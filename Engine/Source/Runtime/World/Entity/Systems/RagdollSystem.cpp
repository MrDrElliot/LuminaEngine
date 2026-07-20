#include "pch.h"
#include "RagdollSystem.h"
#include "Physics/PhysicsScene.h"
#include "Core/Math/Math.h"
#include "Core/Math/Transform.h"
#include "Renderer/MeshData.h"
#include "Animation/Pose.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

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

    void SRagdollSystem::Update(const FSystemContext& SystemContext) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        Physics::IPhysicsScene* Scene = SystemContext.GetPhysicsScene();
        if (Scene == nullptr)
        {
            return;
        }

        const EUpdateStage Stage = SystemContext.GetUpdateStage();
        auto View = SystemContext.CreateView<SRagdollComponent, SSkeletalMeshComponent, STransformComponent>();

        if (Stage == EUpdateStage::PrePhysics)
        {
            for (entt::entity Entity : View)
            {
                SRagdollComponent& Ragdoll = View.get<SRagdollComponent>(Entity);

                // Inactive -> Simulated: build the ragdoll from the current animated pose.
                if (Ragdoll.State == ERagdollState::Simulated && Ragdoll.RealizedState != ERagdollState::Simulated)
                {
                    const SSkeletalMeshComponent& Mesh = View.get<SSkeletalMeshComponent>(Entity);
                    const STransformComponent& Transform = View.get<STransformComponent>(Entity);

                    CSkeleton* SkeletonAsset = ResolveSkeletonAsset(Mesh);
                    FSkeletonResource* Skeleton = SkeletonAsset ? SkeletonAsset->GetSkeletonResource() : nullptr;
                    if (Skeleton == nullptr || Skeleton->GetNumBones() == 0)
                    {
                        continue;
                    }

                    // Seed from the live animation pose if present, else the skeleton's bind pose (so an
                    // un-animated skeletal mesh still ragdolls). Both are skinning matrices (Global*InvBind).
                    const int32 NumBones = Skeleton->GetNumBones();
                    TVector<FMatrix4> BindMatrices;
                    const TVector<FMatrix4>* Source = &Mesh.BoneTransforms;
                    if ((int32)Mesh.BoneTransforms.size() != NumBones)
                    {
                        SkeletonAsset->ComputeBindPoseSkinningMatrices(BindMatrices);
                        Source = &BindMatrices;
                    }

                    // Recover component-space bone globals (Global = Skin * inverse(InvBind)).
                    TVector<FMatrix4> Globals;
                    Globals.resize(NumBones);
                    for (int32 i = 0; i < NumBones; ++i)
                    {
                        Globals[i] = (*Source)[i] * Math::Inverse(Skeleton->GetBone(i).InvBindMatrix);
                    }

                    Physics::FRagdollDesc Desc;
                    Desc.Entity = Entity;
                    Desc.Asset = Ragdoll.PhysicsAsset;
                    Desc.Skeleton = Skeleton;
                    Desc.ComponentBoneGlobals = &Globals;
                    Desc.EntityToWorld = Transform.GetWorldMatrix();
                    Desc.CollisionGroupID = Scene->AllocateRagdollGroupID();

                    Ragdoll.Ragdoll = Scene->CreateRagdoll(Desc);
                    Ragdoll.RealizedState = Ragdoll.Ragdoll ? ERagdollState::Simulated : ERagdollState::Inactive;
                }
                // Simulated -> Inactive: tear the bodies down.
                else if (Ragdoll.State == ERagdollState::Inactive && Ragdoll.RealizedState == ERagdollState::Simulated)
                {
                    Scene->DestroyRagdoll(Ragdoll.Ragdoll);
                    Ragdoll.Ragdoll = nullptr;
                    Ragdoll.RealizedState = ERagdollState::Inactive;
                }
            }
        }
        else if (Stage == EUpdateStage::PostPhysics)
        {
            for (entt::entity Entity : View)
            {
                SRagdollComponent& Ragdoll = View.get<SRagdollComponent>(Entity);
                if (Ragdoll.RealizedState != ERagdollState::Simulated || !Ragdoll.Ragdoll)
                {
                    continue;
                }

                SSkeletalMeshComponent& Mesh = View.get<SSkeletalMeshComponent>(Entity);
                const STransformComponent& Transform = View.get<STransformComponent>(Entity);

                CSkeleton* SkeletonAsset = ResolveSkeletonAsset(Mesh);
                FSkeletonResource* Skeleton = SkeletonAsset ? SkeletonAsset->GetSkeletonResource() : nullptr;
                if (Skeleton == nullptr)
                {
                    continue;
                }

                // Optionally move the entity to follow the ragdoll's root body so the mesh's culling
                // bounds track it; compute the bone readback relative to that same frame.
                FMatrix4 WorldToEntity;
                if (Ragdoll.bDriveEntityFromRoot)
                {
                    FVector3 RootPos; FQuat RootRot;
                    Scene->GetRagdollRootTransform(*Ragdoll.Ragdoll, RootPos, RootRot);

                    FTransform World;
                    World.SetLocation(RootPos);
                    World.SetRotation(RootRot);
                    World.SetScale(Transform.GetWorldScale());
                    ECS::Utils::SetEntityWorldTransform(SystemContext.GetRegistry(), Entity, World);

                    WorldToEntity = Math::Inverse(AnimPose::ComposeTRS(RootPos, RootRot, World.GetScale()));
                }
                else
                {
                    WorldToEntity = Math::Inverse(Transform.GetWorldMatrix());
                }

                Scene->ReadRagdollPose(*Ragdoll.Ragdoll, WorldToEntity, Skeleton, Mesh.BoneTransforms);
                Mesh.bRenderBonesDirty = true;
            }
        }
    }
}
