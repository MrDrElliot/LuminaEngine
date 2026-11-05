#include "RuntimePCH.h"
#include "RagdollSystem.h"
#include "AnimationSystem.h"
#include "World/ECS/Registry.h"
#include "Physics/PhysicsScene.h"
#include "Core/Math/Math.h"
#include "Core/Math/Transform.h"
#include "Renderer/MeshData.h"
#include "Animation/Pose.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    static CSkeleton* ResolveSkeletonAsset(const SSkeletalMeshComponent& Mesh)
    {
        if (!Mesh.SkeletalMesh || !Mesh.SkeletalMesh->Skeleton)
        {
            return nullptr;
        }
        return Mesh.SkeletalMesh->Skeleton.Get();
    }

    // Muscles chase the newest pose the animation system wrote, which is never the physics pose this system wrote back over it.
    static void CaptureMuscleTargets(SRagdollComponent& Ragdoll, const SSkeletalMeshComponent& Mesh)
    {
        CSkeleton* SkeletonAsset = ResolveSkeletonAsset(Mesh);
        const FSkeletonResource* Skeleton = SkeletonAsset ? SkeletonAsset->GetSkeletonResource() : nullptr;
        if (Skeleton == nullptr || !Skeleton->HasBindGlobalMatrices())
        {
            return;
        }

        const int32 NumBones = Skeleton->GetNumBones();
        const bool bFreshPose = Mesh.BoneTransforms.Num() == NumBones && Mesh.PoseSerial == Mesh.PackedPoseSerial
                             && !Mesh.bRenderBonesDirty && Mesh.PoseSerial != Ragdoll.CapturedPoseSerial;
        if (!bFreshPose && Ragdoll.TargetGlobals.Num() == NumBones)
        {
            return;
        }

        Ragdoll.TargetGlobals.SetNum(NumBones);
        for (int32 Bone = 0; Bone < NumBones; ++Bone)
        {
            const FMatrix4& BindGlobal = Skeleton->BindGlobalMatrices[Bone];
            Ragdoll.TargetGlobals[Bone] = bFreshPose ? Mesh.BoneTransforms[Bone] * BindGlobal : BindGlobal;
        }
        Ragdoll.CapturedPoseSerial = Mesh.PoseSerial;
    }

    void SRagdollSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RequireUpdate(EUpdateStage::PostPhysics);
        RunsAfter<SAnimationSystem>();
    }

    bool SRagdollSystem::HasWork(const SSystemContext& Context)
    {
        return AnyQueryHasRows(Ragdolls);
    }

    void SRagdollSystem::OnUpdate(const SSystemContext& Context)
    {
        Physics::IPhysicsQueries* PhysicsQueries = Context.GetPhysics();
        if (PhysicsQueries == nullptr)
        {
            return;
        }

        const EUpdateStage Stage = Context.GetUpdateStage();

        if (Stage == EUpdateStage::PrePhysics)
        {
            Ragdolls.ForEach([&](ECS::FEntity Entity, ECS::TMut<SRagdollComponent> RagdollMut, const SSkeletalMeshComponent& Mesh, const STransformComponent& Transform)
            {
                const SRagdollComponent& Ragdoll = RagdollMut.Get();
                if (Ragdoll.State == ERagdollState::Powered)
                {
                    CaptureMuscleTargets(RagdollMut.GetMut(), Mesh);
                }

                // Limp and powered share one set of bodies, so moving between them only changes what the muscles do.
                if (Ragdoll.State != ERagdollState::Inactive && Ragdoll.RealizedState != ERagdollState::Inactive)
                {
                    RagdollMut.SetIfChanged(&SRagdollComponent::RealizedState, Ragdoll.State);
                }

                // Into physics, so build the ragdoll from the current animated pose.
                if (Ragdoll.State != ERagdollState::Inactive && Ragdoll.RealizedState == ERagdollState::Inactive)
                {
                    CSkeleton* SkeletonAsset = ResolveSkeletonAsset(Mesh);
                    FSkeletonResource* Skeleton = SkeletonAsset ? SkeletonAsset->GetSkeletonResource() : nullptr;
                    if (Skeleton == nullptr || Skeleton->GetNumBones() == 0)
                    {
                        return;
                    }

                    // Seed from the live pose or the bind pose, both of which are skinning matrices.
                    const int32 NumBones = Skeleton->GetNumBones();
                    TVector<FMatrix4> BindMatrices;
                    const TVector<FMatrix4>* Source = &Mesh.BoneTransforms;
                    if (Mesh.BoneTransforms.Num() != NumBones)
                    {
                        SkeletonAsset->ComputeBindPoseSkinningMatrices(BindMatrices);
                        Source = &BindMatrices;
                    }

                    // Recover component-space bone globals (Global = Skin * inverse(InvBind)).
                    TVector<FMatrix4> Globals;
                    Globals.SetNum(NumBones);
                    for (int32 i = 0; i < NumBones; ++i)
                    {
                        Globals[i] = (*Source)[i] * Math::Inverse(Skeleton->GetBone(i).InvBindMatrix);
                    }

                    Context.Defer([Entity, Skeleton, Globals = Move(Globals), EntityToWorld = Transform.GetWorldMatrix()](const SExclusiveContext& Exclusive)
                    {
                        Physics::IPhysicsScene* Scene = Exclusive.GetPhysicsScene();
                        ECS::FRegistry& Registry = Exclusive.GetRegistry();
                        SRagdollComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<SRagdollComponent>(Entity) : nullptr;
                        if (Scene == nullptr || Live == nullptr || Live->State == ERagdollState::Inactive || Live->RealizedState != ERagdollState::Inactive)
                        {
                            return;
                        }

                        Physics::FRagdollDesc Desc;
                        Desc.Entity = Entity;
                        Desc.Asset = Live->PhysicsAsset.Get();
                        Desc.Skeleton = Skeleton;
                        Desc.ComponentBoneGlobals = &Globals;
                        Desc.EntityToWorld = EntityToWorld;
                        Desc.CollisionGroupID = Scene->AllocateRagdollGroupID();

                        Live->Ragdoll = Scene->CreateRagdoll(Desc);
                        Live->RealizedState = Live->Ragdoll ? Live->State : ERagdollState::Inactive;
                    });
                }
                // Out of physics, so tear the bodies down.
                else if (Ragdoll.State == ERagdollState::Inactive && Ragdoll.RealizedState != ERagdollState::Inactive)
                {
                    Context.Defer([Entity](const SExclusiveContext& Exclusive)
                    {
                        Physics::IPhysicsScene* Scene = Exclusive.GetPhysicsScene();
                        ECS::FRegistry& Registry = Exclusive.GetRegistry();
                        SRagdollComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<SRagdollComponent>(Entity) : nullptr;
                        if (Scene == nullptr || Live == nullptr || Live->RealizedState == ERagdollState::Inactive)
                        {
                            return;
                        }
                        Scene->DestroyRagdoll(Live->Ragdoll);
                        Live->Ragdoll = nullptr;
                        Live->RealizedState = ERagdollState::Inactive;
                    });
                }
            });
        }
        else if (Stage == EUpdateStage::PostPhysics)
        {
            Ragdolls.ForEach([&](ECS::FEntity Entity, ECS::TMut<SRagdollComponent> RagdollMut, const SSkeletalMeshComponent& ReadMesh, const STransformComponent& Transform)
            {
                const SRagdollComponent& Ragdoll = RagdollMut.Get();
                if (Ragdoll.RealizedState == ERagdollState::Inactive || !Ragdoll.Ragdoll || !PhysicsQueries->IsRagdollReady(*Ragdoll.Ragdoll))
                {
                    return;
                }

                CSkeleton* SkeletonAsset = ResolveSkeletonAsset(ReadMesh);
                FSkeletonResource* Skeleton = SkeletonAsset ? SkeletonAsset->GetSkeletonResource() : nullptr;
                if (Skeleton == nullptr)
                {
                    return;
                }

                SSkeletalMeshComponent& Mesh = PoseTargets.Get<SSkeletalMeshComponent>(Entity);

                // Optionally follow the ragdoll root so culling bounds track it, and read bones back there, unless the root follows the entity instead.
                const bool bRootFollowsEntity = Ragdoll.State == ERagdollState::Powered && Ragdoll.RootDrive == ERagdollRootDrive::FollowEntity;
                FMatrix4 WorldToEntity;
                if (Ragdoll.bDriveEntityFromRoot && !bRootFollowsEntity)
                {
                    FVector3 RootPos; FQuat RootRot;
                    PhysicsQueries->GetRagdollRootTransform(*Ragdoll.Ragdoll, RootPos, RootRot);

                    FTransform World;
                    World.SetLocation(RootPos);
                    World.SetRotation(RootRot);
                    World.SetScale(Transform.GetWorldScale());
                    DrivenTransforms.Get<STransformComponent>(Entity).SetWorldTransform(World);

                    WorldToEntity = Math::Inverse(AnimPose::ComposeTRS(RootPos, RootRot, World.GetScale()));
                }
                else
                {
                    WorldToEntity = Math::Inverse(Transform.GetWorldMatrix());
                }

                PhysicsQueries->ReadRagdollPose(*Ragdoll.Ragdoll, WorldToEntity, Skeleton, Mesh.BoneTransforms);
                Mesh.bRenderBonesDirty = true;
            });
        }
    }
}
