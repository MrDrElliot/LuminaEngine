#include "RuntimePCH.h"
#include "FootPlacementSystem.h"

#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Physics/PhysicsScene.h"
#include "Renderer/SkeletonResource.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/FootPlacementComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SystemResources.h"

namespace Lumina
{
    FSystemAccess SFootPlacementSystem::Access = FSystemAccess{}
        .Write<SFootPlacementComponent, SAnimationGraphComponent>()
        .Read<SSkeletalMeshComponent, STransformComponent>();

    namespace
    {
        // Every parameter this component publishes, in the order the bindings are cached.
        int32 CountBindings(const SFootPlacementComponent& Foot)
        {
            return 3 + (int32)Foot.Feet.size() * 6;
        }

        void ResolveBindings(SFootPlacementComponent& Placement, const CAnimationGraph& Graph, const FSkeletonResource& Skeleton)
        {
            CStruct* ParameterStruct = Graph.GetParameterStruct();

            const auto Bind = [&](const FName& Name) -> FAnimGraphParamBinding
            {
                FAnimGraphParamBinding Binding;
                if (ParameterStruct == nullptr || Name.IsNone())
                {
                    return Binding;
                }

                FProperty* Property = ParameterStruct->GetProperty(Name);
                if (Property == nullptr || Property->HasSetterOrGetter())
                {
                    return Binding;
                }

                Binding.Type   = AnimParamValueTypeFromProperty(Property);
                Binding.Offset = Property->Offset;
                return Binding;
            };

            Placement.Bindings.clear();
            Placement.Bindings.reserve(CountBindings(Placement));

            Placement.Bindings.push_back(Bind(Placement.PelvisOffsetXParameter));
            Placement.Bindings.push_back(Bind(Placement.PelvisOffsetYParameter));
            Placement.Bindings.push_back(Bind(Placement.PelvisOffsetZParameter));

            for (SFootPlacementFoot& Foot : Placement.Feet)
            {
                Placement.Bindings.push_back(Bind(Foot.OffsetXParameter));
                Placement.Bindings.push_back(Bind(Foot.OffsetYParameter));
                Placement.Bindings.push_back(Bind(Foot.OffsetZParameter));
                Placement.Bindings.push_back(Bind(Foot.NormalXParameter));
                Placement.Bindings.push_back(Bind(Foot.NormalYParameter));
                Placement.Bindings.push_back(Bind(Foot.NormalZParameter));

                Foot.BoneIndex = Skeleton.FindBoneIndex(Foot.FootBone);
                Foot.BindMatrix = (Foot.BoneIndex != INDEX_NONE)
                    ? Math::Inverse(Skeleton.GetBone(Foot.BoneIndex).InvBindMatrix)
                    : FMatrix4(1.0f);
            }

            Placement.BoundGraph    = &Graph;
            Placement.BoundSkeleton = &Skeleton;
        }

        void WriteVector(SFootPlacementComponent& Placement, uint8* Memory, int32 FirstBinding, const FVector3& Value)
        {
            if (Memory == nullptr)
            {
                return;
            }

            const float Components[3] = { Value.x, Value.y, Value.z };
            for (int32 i = 0; i < 3; ++i)
            {
                const FAnimGraphParamBinding& Binding = Placement.Bindings[FirstBinding + i];
                if (Binding.IsResolved())
                {
                    WriteAnimParamScalar(Memory, Binding, Components[i]);
                }
            }
        }

        FORCEINLINE float SmoothingAlpha(float HalfLife, float DeltaTime)
        {
            return (HalfLife > 1e-5f && DeltaTime > 0.0f)
                ? 1.0f - Math::Exp(-DeltaTime * 0.6931472f / HalfLife)
                : 1.0f;
        }
    }

    void SFootPlacementSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        auto View = Context.CreateView<SFootPlacementComponent, SAnimationGraphComponent, SSkeletalMeshComponent, STransformComponent>(entt::exclude<SDisabledTag>);
        auto Handle = View.handle();
        if (Handle == nullptr || Handle->empty())
        {
            return;
        }

        Physics::IPhysicsScene* Scene = Context.GetPhysicsScene();
        if (Scene == nullptr)
        {
            return;
        }

        const float DeltaTime = (float)Context.GetDeltaTime();

        TVector<entt::entity> Entities(Handle->begin(), Handle->end());
        TVector<uint32> Bodies(Entities.size());
        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            Bodies[i] = Context.GetEntityBodyID(Entities[i]);
        }

        // Queries are read-only and PrePhysics never overlaps the step, so tracing here is safe.
        Task::ParallelFor((uint32)Entities.size(), [&](uint32 Index)
        {
            const entt::entity Entity = Entities[Index];

            SFootPlacementComponent&  Placement = View.get<SFootPlacementComponent>(Entity);
            SAnimationGraphComponent& AnimGraph = View.get<SAnimationGraphComponent>(Entity);
            const SSkeletalMeshComponent& Mesh  = View.get<SSkeletalMeshComponent>(Entity);
            const STransformComponent& Transform = View.get<STransformComponent>(Entity);

            CAnimationGraph* Graph = AnimGraph.Graph.Get();
            CSkeleton* SkeletonAsset = SkeletalUtils::GetSkeletonAsset(Mesh);
            if (Graph == nullptr || SkeletonAsset == nullptr)
            {
                return;
            }

            const FSkeletonResource* Skeleton = SkeletonAsset->GetSkeletonResource();
            if (Skeleton == nullptr)
            {
                return;
            }

            if (Placement.BoundGraph != Graph || Placement.BoundSkeleton != Skeleton ||
                Placement.Bindings.size() != (SIZE_T)CountBindings(Placement))
            {
                ResolveBindings(Placement, *Graph, *Skeleton);
            }

            AnimGraph.EnsureParametersInitialized();
            uint8* ParameterMemory = (uint8*)AnimGraph.GetParameterMemory();

            const float Alpha = SmoothingAlpha(Placement.SmoothingHalfLife, DeltaTime);

            const VTransform WorldTransform = Transform.GetWorldTransformCached();
            const FVector3 WorldLocation = WorldTransform.GetLocation();
            const FVector3 WorldScale    = WorldTransform.GetScale();
            const FQuat WorldRotation    = WorldTransform.GetRotation();
            const FQuat InverseRotation  = Math::Conjugate(WorldRotation);

            const float UpLength = Math::Length(Placement.UpAxis);
            const FVector3 Up = UpLength > 1e-5f ? Placement.UpAxis / UpLength : FVector3(0.0f, 1.0f, 0.0f);

            // Deepest foot of the frame, which the pelvis follows so the other leg can still reach.
            float LowestAlongUp = 0.0f;

            for (SIZE_T FootIndex = 0; FootIndex < Placement.Feet.size(); ++FootIndex)
            {
                SFootPlacementFoot& Foot = Placement.Feet[FootIndex];

                FVector3 TargetOffset(0.0f);
                FVector3 TargetNormal = Up;

                const bool bCanTrace = Placement.bEnabled &&
                    Foot.BoneIndex != INDEX_NONE &&
                    Foot.BoneIndex < (int32)Mesh.BoneTransforms.size();

                if (bCanTrace)
                {
                    // Last frame's pose, since this frame's has not been evaluated yet.
                    const FMatrix4 FootSkinning = Mesh.BoneTransforms[Foot.BoneIndex];
                    const FMatrix4 FootGlobal = FootSkinning * Foot.BindMatrix;
                    const FVector3 FootComponent = FVector3(FootGlobal[3]);
                    const FVector3 FootWorld = WorldLocation + WorldRotation * (FootComponent * WorldScale);

                    SRayCastSettings Ray;
                    Ray.Start = FootWorld + Up * Placement.TraceUpDistance;
                    Ray.End   = FootWorld - Up * Placement.TraceDownDistance;
                    Ray.LayerMask = Placement.TraceLayerMask;
                    Ray.IgnoreBodies.push_back(Bodies[Index]);

                    if (const TOptional<SRayResult> Hit = Scene->CastRay(Ray))
                    {
                        const FVector3 Desired = Hit->Location + Up * Foot.SoleHeight;
                        FVector3 WorldOffset = Desired - FootWorld;

                        const float OffsetLength = Math::Length(WorldOffset);
                        if (OffsetLength > Placement.MaxOffset && OffsetLength > 1e-5f)
                        {
                            WorldOffset = WorldOffset * (Placement.MaxOffset / OffsetLength);
                        }

                        TargetOffset = InverseRotation * WorldOffset;
                        TargetNormal = Math::Normalize(InverseRotation * Hit->Normal);
                    }
                }

                Foot.SmoothedOffset = Foot.SmoothedOffset + (TargetOffset - Foot.SmoothedOffset) * Alpha;
                Foot.SmoothedNormal = Math::Normalize(Foot.SmoothedNormal + (TargetNormal - Foot.SmoothedNormal) * Alpha);

                LowestAlongUp = Math::Min(LowestAlongUp, Math::Dot(Foot.SmoothedOffset, Up));
            }

            const FVector3 TargetPelvis = Up * LowestAlongUp;
            Placement.SmoothedPelvisOffset = Placement.SmoothedPelvisOffset +
                (TargetPelvis - Placement.SmoothedPelvisOffset) * Alpha;

            WriteVector(Placement, ParameterMemory, 0, Placement.SmoothedPelvisOffset);

            // Each foot is published relative to the pelvis, which has already moved to meet the lowest.
            for (SIZE_T FootIndex = 0; FootIndex < Placement.Feet.size(); ++FootIndex)
            {
                const SFootPlacementFoot& Foot = Placement.Feet[FootIndex];
                const int32 FirstBinding = 3 + (int32)FootIndex * 6;

                WriteVector(Placement, ParameterMemory, FirstBinding, Foot.SmoothedOffset - Placement.SmoothedPelvisOffset);
                WriteVector(Placement, ParameterMemory, FirstBinding + 3, Foot.SmoothedNormal);
            }
        });
    }
}
