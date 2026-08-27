#include "RuntimePCH.h"
#include "SkeletonDebugDraw.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Renderer/ImmediateLineRenderer.h"
#include "Renderer/MeshData.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/DebugDrawSystem.h"
#include "Renderer/SkeletonResource.h"

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

        const bool bHasBinds = Skeleton->HasBindGlobalMatrices();

        // Live pose has Global*InvBind; recover model-space by undoing InvBind.
        if ((int32)BoneTransforms.size() == NumBones)
        {
            for (int32 i = 0; i < NumBones; ++i)
            {
                const FMatrix4 Bind = bHasBinds ? Skeleton->BindGlobalMatrices[i]
                                                : Math::Inverse(Skeleton->GetBone(i).InvBindMatrix);
                OutGlobals[i] = BoneTransforms[i] * Bind;
            }
        }
        else if (bHasBinds)
        {
            for (int32 i = 0; i < NumBones; ++i)
            {
                OutGlobals[i] = Skeleton->BindGlobalMatrices[i];
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
        constexpr uint32 kJointSegments   = 8;
        constexpr uint32 kJointLines      = kJointSegments * 3;
        constexpr uint32 kOctahedralLines = 12;

        // How much of a bone gets drawn, chosen per mesh from its camera distance.
        enum class EDetail : uint8
        {
            Full,    // octahedral bones, joint spheres, axes
            Shapes,  // octahedral bones only
            Sticks,  // one line per bone
        };

        // The joint ring is the same circle for every bone, so its trig is resolved once per process.
        struct FJointRing
        {
            float Cos[kJointSegments + 1];
            float Sin[kJointSegments + 1];

            FJointRing()
            {
                for (uint32 i = 0; i <= kJointSegments; ++i)
                {
                    const float Angle = Math::TwoPi<float>() * (float)i / (float)kJointSegments;
                    Cos[i] = Math::Cos(Angle);
                    Sin[i] = Math::Sin(Angle);
                }
            }
        };

        const FJointRing GJointRing;

        FORCEINLINE void WriteLine(FSimpleElementVertex*& V, const FVector3& A, const FVector3& B, uint32 Color)
        {
            V[0] = FSimpleElementVertex{ A, Color };
            V[1] = FSimpleElementVertex{ B, Color };
            V += 2;
        }

        // Every reserved vertex has to be written, so unusable geometry burns its quota on pad verts.
        FORCEINLINE void WritePadLines(FSimpleElementVertex*& V, uint32 LineCount)
        {
            const uint32 VertexCount = LineCount * 2u;
            for (uint32 i = 0; i < VertexCount; ++i)
            {
                V[i] = FImmediateLineRenderer::MakePadVertex();
            }
            V += VertexCount;
        }

        void WriteOctahedralBone(FSimpleElementVertex*& V, const FVector3& From, const FVector3& To, uint32 Color)
        {
            const FVector3 Axis  = To - From;
            const float    LenSq = Math::Dot(Axis, Axis);
            if (LenSq < 1e-10f)
            {
                WritePadLines(V, kOctahedralLines);
                return;
            }

            const float    Length = Math::Sqrt(LenSq);
            const FVector3 Dir    = Axis / Length;
            FVector3       Up     = (fabsf(Dir.y) < 0.99f) ? FVector3(0, 1, 0) : FVector3(1, 0, 0);
            const FVector3 Side   = Math::Normalize(Math::Cross(Dir, Up));
            Up                    = Math::Normalize(Math::Cross(Side, Dir));

            const float    RidgeRadius = Math::Clamp(Length * 0.10f, 0.006f, 0.05f);
            const FVector3 Center      = From + Dir * (Length * 0.18f);

            const FVector3 R[4] =
            {
                Center + Side * RidgeRadius,
                Center + Up   * RidgeRadius,
                Center - Side * RidgeRadius,
                Center - Up   * RidgeRadius,
            };

            for (uint32 k = 0; k < 4; ++k)
            {
                WriteLine(V, From, R[k], Color);
                WriteLine(V, R[k], To, Color);
                WriteLine(V, R[k], R[(k + 1) & 3], Color);
            }
        }

        void WriteJoint(FSimpleElementVertex*& V, const FVector3& Center, float Radius, uint32 Color)
        {
            for (uint32 i = 0; i < kJointSegments; ++i)
            {
                const float C0 = GJointRing.Cos[i] * Radius;
                const float S0 = GJointRing.Sin[i] * Radius;
                const float C1 = GJointRing.Cos[i + 1] * Radius;
                const float S1 = GJointRing.Sin[i + 1] * Radius;

                WriteLine(V, Center + FVector3(C0, S0, 0.0f), Center + FVector3(C1, S1, 0.0f), Color);
                WriteLine(V, Center + FVector3(C0, 0.0f, S0), Center + FVector3(C1, 0.0f, S1), Color);
                WriteLine(V, Center + FVector3(0.0f, C0, S0), Center + FVector3(0.0f, C1, S1), Color);
            }
        }

        // One visible skeletal mesh, resolved on the game thread so the emit pass touches no ECS state.
        struct FSkeletonJob
        {
            const FSkeletonResource* Skeleton  = nullptr;
            const TVector<FMatrix4>* Pose      = nullptr;
            FMatrix4                 MeshWorld = FMatrix4(1.0f);
            EDetail                  Detail    = EDetail::Full;
        };

        struct FEmitScratch
        {
            TVector<FVector3> Positions;
            TVector<FMatrix4> Bases;
        };

        thread_local FEmitScratch GEmitScratch;

        void EmitSkeleton(FImmediateLineRenderer& Lines, FImmediateLineRenderer::EChannel Channel,
                          const FSkeletonJob& Job, const FOptions& Options, FEmitScratch& Scratch)
        {
            const FSkeletonResource& Skeleton = *Job.Skeleton;
            const int32              NumBones = Skeleton.GetNumBones();
            const TVector<FMatrix4>& Binds    = Skeleton.BindGlobalMatrices;

            const bool bFull   = (Job.Detail == EDetail::Full);
            const bool bAxes   = Options.bAxes && bFull;
            const bool bJoints = Options.bJoints && bFull;
            const bool bShaped = Options.bOctahedral && (Job.Detail != EDetail::Sticks);

            Scratch.Positions.resize(NumBones);
            if (bAxes)
            {
                Scratch.Bases.resize(NumBones);
            }

            uint32 NumParented = 0;
            for (int32 i = 0; i < NumBones; ++i)
            {
                if (bAxes)
                {
                    const FMatrix4 World = Job.Pose ? (Job.MeshWorld * ((*Job.Pose)[i] * Binds[i]))
                                                    : (Job.MeshWorld * Binds[i]);
                    Scratch.Bases[i]     = World;
                    Scratch.Positions[i] = FVector3(World[3]);
                }
                else
                {
                    // Only the bone origin is wanted, so the two matrix products collapse to two mat4*vec4.
                    const FVector4 Model = Job.Pose ? ((*Job.Pose)[i] * Binds[i][3]) : Binds[i][3];
                    Scratch.Positions[i] = FVector3(Job.MeshWorld * Model);
                }

                NumParented += (Skeleton.GetBone(i).ParentIndex >= 0) ? 1u : 0u;
            }

            const uint32 BoneLines  = Options.bBones ? (bShaped ? kOctahedralLines : 1u) : 0u;
            const uint32 JointLines = bJoints ? kJointLines : 0u;
            const uint32 AxisLines  = bAxes ? 3u : 0u;
            const uint32 TotalLines = NumParented * BoneLines + (uint32)NumBones * (JointLines + AxisLines);
            if (TotalLines == 0)
            {
                return;
            }

            FSimpleElementVertex* V = Lines.AllocLines(TotalLines, Channel);
            if (V == nullptr)
            {
                return;
            }

            const uint32 BoneColor  = PackColor(Options.BoneColor);
            const uint32 RootColor  = PackColor(Options.RootColor);
            const uint32 JointColor = PackColor(Options.JointColor);
            const uint32 AxisXColor = PackColor(FVector4(1.00f, 0.25f, 0.25f, 1.0f));
            const uint32 AxisYColor = PackColor(FVector4(0.35f, 1.00f, 0.35f, 1.0f));
            const uint32 AxisZColor = PackColor(FVector4(0.35f, 0.55f, 1.00f, 1.0f));

            for (int32 i = 0; i < NumBones; ++i)
            {
                const int32    ParentIndex = Skeleton.GetBone(i).ParentIndex;
                const FVector3 Position    = Scratch.Positions[i];

                if (BoneLines > 0 && ParentIndex >= 0)
                {
                    const FVector3 ParentPos = Scratch.Positions[ParentIndex];
                    if (bShaped)
                    {
                        WriteOctahedralBone(V, ParentPos, Position, BoneColor);
                    }
                    else
                    {
                        WriteLine(V, ParentPos, Position, BoneColor);
                    }
                }

                if (bJoints)
                {
                    const bool  bRoot  = (ParentIndex < 0);
                    const float Radius = bRoot ? Options.JointRadius * 1.8f : Options.JointRadius;
                    WriteJoint(V, Position, Radius, bRoot ? RootColor : JointColor);
                }

                if (bAxes)
                {
                    const FMatrix4& World = Scratch.Bases[i];
                    WriteLine(V, Position, Position + Math::Normalize(FVector3(World[0])) * Options.AxisLength, AxisXColor);
                    WriteLine(V, Position, Position + Math::Normalize(FVector3(World[1])) * Options.AxisLength, AxisYColor);
                    WriteLine(V, Position, Position + Math::Normalize(FVector3(World[2])) * Options.AxisLength, AxisZColor);
                }
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
                const FVector3 Axis      = Position - ParentPos;
                const float    LenSq     = Math::Dot(Axis, Axis);

                if (Options.bOctahedral && LenSq >= 1e-10f)
                {
                    const float    Length = Math::Sqrt(LenSq);
                    const FVector3 Dir    = Axis / Length;
                    FVector3       Up     = (fabsf(Dir.y) < 0.99f) ? FVector3(0, 1, 0) : FVector3(1, 0, 0);
                    const FVector3 Side   = Math::Normalize(Math::Cross(Dir, Up));
                    Up                    = Math::Normalize(Math::Cross(Side, Dir));

                    const float    RidgeRadius = Math::Clamp(Length * 0.10f, 0.006f, 0.05f);
                    const FVector3 Center      = ParentPos + Dir * (Length * 0.18f);
                    const FVector3 R[4] =
                    {
                        Center + Side * RidgeRadius,
                        Center + Up   * RidgeRadius,
                        Center - Side * RidgeRadius,
                        Center - Up   * RidgeRadius,
                    };

                    for (int k = 0; k < 4; ++k)
                    {
                        DrawInterface->DrawLine(ParentPos, R[k], Options.BoneColor, Options.BoneThickness, Options.bDepthTest);
                        DrawInterface->DrawLine(R[k], Position, Options.BoneColor, Options.BoneThickness, Options.bDepthTest);
                        DrawInterface->DrawLine(R[k], R[(k + 1) & 3], Options.BoneColor, Options.BoneThickness, Options.bDepthTest);
                    }
                }
                else if (!Options.bOctahedral)
                {
                    DrawInterface->DrawLine(ParentPos, Position, Options.BoneColor, Options.BoneThickness, Options.bDepthTest);
                }
            }

            if (Options.bJoints)
            {
                const bool     bRoot  = (Bone.ParentIndex < 0);
                const FVector4 Color  = bRoot ? Options.RootColor : Options.JointColor;
                const float    Radius = bRoot ? Options.JointRadius * 1.8f : Options.JointRadius;
                DrawInterface->DrawSphere(Position, Radius, Color, kJointSegments, Options.BoneThickness, Options.bDepthTest);
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
        // Reading a world transform can resolve a dirty chain, which mutates, so this stays serial.
        template <typename TFunc>
        void ForEachVisibleSkeleton(CWorld* World, const FDebugDrawState* State, const FOptions& Options, TFunc&& Callback)
        {
            auto View = World->View<SSkeletalMeshComponent, STransformComponent>();

            for (ECS::FEntity Entity : View)
            {
                const SSkeletalMeshComponent& Mesh = View.Get<SSkeletalMeshComponent>(Entity);
                if (!Mesh.SkeletalMesh.IsValid())
                {
                    continue;
                }

                CSkeletalMesh* SkeletalMesh = Mesh.SkeletalMesh;
                if (!SkeletalMesh->Skeleton.IsValid())
                {
                    continue;
                }

                const FSkeletonResource* Skeleton = SkeletalMesh->Skeleton->GetSkeletonResource();
                if (Skeleton == nullptr || Skeleton->GetNumBones() == 0 || !Skeleton->HasBindGlobalMatrices())
                {
                    continue;
                }

                FSkeletonJob Job;
                Job.MeshWorld = View.Get<STransformComponent>(Entity).GetWorldMatrix();
                Job.Skeleton  = Skeleton;
                Job.Pose      = ((int32)Mesh.BoneTransforms.size() == Skeleton->GetNumBones()) ? &Mesh.BoneTransforms : nullptr;

                const FAABB LocalBounds = SkeletalMesh->GetAABB();
                if (State != nullptr && LocalBounds.IsValid())
                {
                    const FAABB WorldBounds = LocalBounds.ToWorld(Job.MeshWorld);
                    if (!DebugDraw::ShouldDraw(*State, WorldBounds))
                    {
                        continue;
                    }

                    const FVector3 Extent = (WorldBounds.Max - WorldBounds.Min) * 0.5f;
                    const float    Radius = Math::Sqrt(Math::Dot(Extent, Extent));
                    const FVector3 Delta  = WorldBounds.GetCenter() - State->ViewOrigin;
                    const float    Reach  = Math::Max(0.0f, Math::Sqrt(Math::Dot(Delta, Delta)) - Radius);

                    Job.Detail = (Reach <= Options.JointDistance) ? EDetail::Full
                               : (Reach <= Options.ShapeDistance) ? EDetail::Shapes
                                                                  : EDetail::Sticks;
                }

                Callback(Job);
            }
        }
    }

    void DrawWorldSkeletons(CWorld* World, IPrimitiveDrawInterface* DrawInterface, const FOptions& Options)
    {
        LUMINA_PROFILE_SCOPE();

        if (World == nullptr)
        {
            return;
        }

        const FDebugDrawState*  State = DebugDraw::GetState(World);
        FImmediateLineRenderer* Lines = (State != nullptr) ? DebugDraw::GetLines(World) : nullptr;

        if (Lines == nullptr)
        {
            // A world that has a state but no sink is one the master switch or a missing view turned off.
            if (State != nullptr || DrawInterface == nullptr)
            {
                return;
            }

            static const TVector<FMatrix4> NoPose;
            TVector<FMatrix4>              Globals;
            ForEachVisibleSkeleton(World, nullptr, Options, [&](const FSkeletonJob& Job)
            {
                ComputeGlobalBoneTransforms(Job.Skeleton, Job.Pose ? *Job.Pose : NoPose, Globals);
                DrawSkeleton(DrawInterface, Job.Skeleton, Globals, Job.MeshWorld, Options);
            });
            return;
        }

        // Game thread only, so one persistent list keeps the gather off the allocator every frame.
        static TVector<FSkeletonJob> Jobs;
        Jobs.clear();
        ForEachVisibleSkeleton(World, State, Options, [&](const FSkeletonJob& Job) { Jobs.push_back(Job); });

        if (Jobs.empty())
        {
            return;
        }

        const FImmediateLineRenderer::EChannel Channel = Options.bDepthTest
            ? FImmediateLineRenderer::DepthTested
            : FImmediateLineRenderer::XRay;

        Task::ParallelFor((uint32)Jobs.size(), [&](const Task::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                EmitSkeleton(*Lines, Channel, Jobs[i], Options, GEmitScratch);
            }
        }, 4);
    }

    void GatherWorldBoneLabels(CWorld* World, const FVector3& ViewOrigin, float MaxDistance,
                               TVector<FBoneLabel>& OutLabels, int32 MaxLabels)
    {
        LUMINA_PROFILE_SCOPE();

        if (World == nullptr || MaxLabels <= 0)
        {
            return;
        }

        const FDebugDrawState* State     = DebugDraw::GetState(World);
        const float            MaxDistSq = MaxDistance * MaxDistance;

        ForEachVisibleSkeleton(World, State, FOptions{}, [&](const FSkeletonJob& Job)
        {
            const FSkeletonResource& Skeleton = *Job.Skeleton;
            const int32              NumBones = Skeleton.GetNumBones();

            for (int32 i = 0; i < NumBones && (int32)OutLabels.size() < MaxLabels; ++i)
            {
                const FVector4 Model = Job.Pose ? ((*Job.Pose)[i] * Skeleton.BindGlobalMatrices[i][3])
                                                : Skeleton.BindGlobalMatrices[i][3];
                const FVector3 Position = FVector3(Job.MeshWorld * Model);

                const FVector3 Delta = Position - ViewOrigin;
                if (MaxDistSq > 0.0f && Math::Dot(Delta, Delta) > MaxDistSq)
                {
                    continue;
                }

                OutLabels.push_back({ Skeleton.GetBone(i).Name, Position });
            }
        });
    }
}
