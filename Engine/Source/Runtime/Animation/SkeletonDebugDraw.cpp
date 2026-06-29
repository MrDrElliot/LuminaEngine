#include "pch.h"
#include "SkeletonDebugDraw.h"

#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Renderer/MeshData.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "World/World.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina::SkeletonDebugDraw
{
    void ComputeGlobalBoneTransforms(const FSkeletonResource* Skeleton,
                                     const TVector<FMatrix4>& BoneTransforms,
                                     TVector<FMatrix4>& OutGlobals)
    {
        const int32 NumBones = Skeleton ? Skeleton->GetNumBones() : 0;
        OutGlobals.resize(NumBones);
        if (NumBones == 0)
        {
            return;
        }

        // Live pose has Global*InvBind; recover model-space by undoing InvBind.
        if ((int32)BoneTransforms.size() == NumBones)
        {
            for (int32 i = 0; i < NumBones; ++i)
            {
                OutGlobals[i] = BoneTransforms[i] * Math::Inverse(Skeleton->GetBone(i).InvBindMatrix);
            }
        }
        else
        {
            for (int32 i = 0; i < NumBones; ++i)
            {
                const FSkeletonResource::FBoneInfo& Bone = Skeleton->GetBone(i);
                OutGlobals[i] = (Bone.ParentIndex >= 0)
                    ? OutGlobals[Bone.ParentIndex] * Bone.LocalTransform
                    : Bone.LocalTransform;
            }
        }
    }

    namespace
    {
        void DrawOctahedralBone(IPrimitiveDrawInterface* DI, const FVector3& From, const FVector3& To,
                                const FVector4& Color, float Thickness, bool bDepthTest)
        {
            const FVector3 Axis = To - From;
            const float Length   = Math::Length(Axis);
            if (Length < 1e-5f)
            {
                return;
            }

            const FVector3 Dir  = Axis / Length;
            FVector3 Up         = (fabsf(Dir.y) < 0.99f) ? FVector3(0, 1, 0) : FVector3(1, 0, 0);
            const FVector3 Side = Math::Normalize(Math::Cross(Dir, Up));
            Up                   = Math::Normalize(Math::Cross(Side, Dir));

            const float RidgeDist   = Length * 0.18f;
            const float RidgeRadius = Math::Clamp(Length * 0.10f, 0.006f, 0.05f);
            const FVector3 Center  = From + Dir * RidgeDist;

            const FVector3 R[4] =
            {
                Center + Side * RidgeRadius,
                Center + Up   * RidgeRadius,
                Center - Side * RidgeRadius,
                Center - Up   * RidgeRadius,
            };

            for (int k = 0; k < 4; ++k)
            {
                DI->DrawLine(From, R[k], Color, Thickness, bDepthTest);          // base fan
                DI->DrawLine(R[k], To,   Color, Thickness, bDepthTest);          // tip fan
                DI->DrawLine(R[k], R[(k + 1) & 3], Color, Thickness, bDepthTest); // ridge ring
            }
        }
    }

    void DrawSkeleton(IPrimitiveDrawInterface* DrawInterface,
                      const FSkeletonResource* Skeleton,
                      const TVector<FMatrix4>& GlobalBoneTransforms,
                      const FMatrix4& MeshWorldMatrix,
                      const FOptions& Options)
    {
        if (DrawInterface == nullptr || Skeleton == nullptr)
        {
            return;
        }

        const int32 NumBones = Skeleton->GetNumBones();
        if ((int32)GlobalBoneTransforms.size() != NumBones)
        {
            return;
        }

        for (int32 i = 0; i < NumBones; ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = Skeleton->GetBone(i);
            const FMatrix4 WorldBone = MeshWorldMatrix * GlobalBoneTransforms[i];
            const FVector3 Position  = FVector3(WorldBone[3]);

            if (Options.bBones && Bone.ParentIndex >= 0)
            {
                const FVector3 ParentPos = FVector3((MeshWorldMatrix * GlobalBoneTransforms[Bone.ParentIndex])[3]);
                if (Options.bOctahedral)
                {
                    DrawOctahedralBone(DrawInterface, ParentPos, Position, Options.BoneColor, Options.BoneThickness, Options.bDepthTest);
                }
                else
                {
                    DrawInterface->DrawLine(ParentPos, Position, Options.BoneColor, Options.BoneThickness, Options.bDepthTest);
                }
            }

            if (Options.bJoints)
            {
                const bool  bRoot  = (Bone.ParentIndex < 0);
                const FVector4 Color = bRoot ? Options.RootColor : Options.JointColor;
                const float Radius = bRoot ? Options.JointRadius * 1.8f : Options.JointRadius;
                DrawInterface->DrawSphere(Position, Radius, Color, 8, Options.BoneThickness, Options.bDepthTest);
            }

            if (Options.bAxes)
            {
                const FVector3 AxisX = Math::Normalize(FVector3(WorldBone[0])) * Options.AxisLength;
                const FVector3 AxisY = Math::Normalize(FVector3(WorldBone[1])) * Options.AxisLength;
                const FVector3 AxisZ = Math::Normalize(FVector3(WorldBone[2])) * Options.AxisLength;
                DrawInterface->DrawLine(Position, Position + AxisX, FVector4(1.0f, 0.25f, 0.25f, 1.0f), 1.5f, Options.bDepthTest);
                DrawInterface->DrawLine(Position, Position + AxisY, FVector4(0.35f, 1.0f, 0.35f, 1.0f), 1.5f, Options.bDepthTest);
                DrawInterface->DrawLine(Position, Position + AxisZ, FVector4(0.35f, 0.55f, 1.0f, 1.0f), 1.5f, Options.bDepthTest);
            }
        }
    }

    namespace
    {
        // Shared iteration: invoke Callback(skeleton, meshWorld, globals) for every drawable
        // skeletal mesh in the world. Keeps DrawWorldSkeletons / GatherWorldBoneLabels in sync.
        template <typename TFunc>
        void ForEachSkeletalMesh(CWorld* World, TFunc&& Callback)
        {
            if (World == nullptr)
            {
                return;
            }

            auto View = World->View<SSkeletalMeshComponent, STransformComponent>();

            TVector<FMatrix4> Globals;
            for (entt::entity Entity : View)
            {
                const SSkeletalMeshComponent& Mesh = View.get<SSkeletalMeshComponent>(Entity);
                if (!Mesh.SkeletalMesh.IsValid())
                {
                    continue;
                }

                CSkeletalMesh* SkeletalMesh = Mesh.SkeletalMesh;
                if (!SkeletalMesh->Skeleton.IsValid())
                {
                    continue;
                }

                FSkeletonResource* Skeleton = SkeletalMesh->Skeleton->GetSkeletonResource();
                if (Skeleton == nullptr || Skeleton->GetNumBones() == 0)
                {
                    continue;
                }

                const FMatrix4 MeshWorld = View.get<STransformComponent>(Entity).GetWorldMatrix();
                ComputeGlobalBoneTransforms(Skeleton, Mesh.BoneTransforms, Globals);
                Callback(Skeleton, MeshWorld, Globals);
            }
        }
    }

    void DrawWorldSkeletons(CWorld* World, IPrimitiveDrawInterface* DrawInterface, const FOptions& Options)
    {
        if (DrawInterface == nullptr)
        {
            return;
        }

        ForEachSkeletalMesh(World, [&](const FSkeletonResource* Skeleton, const FMatrix4& MeshWorld, const TVector<FMatrix4>& Globals)
        {
            DrawSkeleton(DrawInterface, Skeleton, Globals, MeshWorld, Options);
        });
    }

    void GatherWorldBoneLabels(CWorld* World, TVector<FBoneLabel>& OutLabels)
    {
        ForEachSkeletalMesh(World, [&](const FSkeletonResource* Skeleton, const FMatrix4& MeshWorld, const TVector<FMatrix4>& Globals)
        {
            const int32 NumBones = Skeleton->GetNumBones();
            for (int32 i = 0; i < NumBones; ++i)
            {
                OutLabels.push_back({ Skeleton->GetBone(i).Name, FVector3((MeshWorld * Globals[i])[3]) });
            }
        });
    }
}
