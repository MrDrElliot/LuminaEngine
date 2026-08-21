#include "EditorAssetDropHandlers.h"

#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Log/Log.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace
    {
        // Retargets an existing component, otherwise spawns, mirroring the static-mesh handler.
        entt::entity DropSkeletalMesh(CWorld* World, CSkeletalMesh* Mesh, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget)
        {
            if (Mesh == nullptr || World == nullptr)
            {
                return entt::null;
            }

            entt::registry& Registry = ECS::GetWorldRegistry(*World);
            if (DropTarget != entt::null && Registry.valid(DropTarget))
            {
                if (SSkeletalMeshComponent* Existing = Registry.try_get<SSkeletalMeshComponent>(DropTarget))
                {
                    Existing->SetSkeletalMesh(Mesh);
                    return DropTarget;
                }
            }

            entt::entity Entity = World->ConstructEntity(Mesh->GetName(), SpawnTransform);
            SSkeletalMeshComponent& MeshComponent = Registry.emplace<SSkeletalMeshComponent>(Entity);
            MeshComponent.SetSkeletalMesh(Mesh);

            if (bAttachToTarget && DropTarget != entt::null && Registry.valid(DropTarget))
            {
                ECS::Utils::ReparentEntity(Registry, Entity, DropTarget);
            }

            return Entity;
        }
    }

    FEditorAssetDropRegistry& FEditorAssetDropRegistry::Get()
    {
        static FEditorAssetDropRegistry Instance;
        static bool bRegistered = false;
        if (!bRegistered)
        {
            bRegistered = true;

            // Dropped on an existing mesh entity it just replaces that entity's mesh asset.
            Instance.Register(CStaticMesh::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    CStaticMesh* Mesh = Cast<CStaticMesh>(Asset);
                    if (Mesh == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        if (SStaticMeshComponent* Existing = Registry.try_get<SStaticMeshComponent>(DropTarget))
                        {
                            Existing->SetStaticMesh(Mesh);
                            return DropTarget;
                        }
                    }

                    entt::entity Entity = World->ConstructEntity(Mesh->GetName(), SpawnTransform);
                    SStaticMeshComponent& MeshComponent = Registry.emplace<SStaticMeshComponent>(Entity);
                    MeshComponent.SetStaticMesh(Mesh);

                    if (bAttachToTarget && DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        ECS::Utils::ReparentEntity(Registry, Entity, DropTarget);
                    }

                    return Entity;
                });

            // A material is only meaningful on an existing mesh entity, and sets slot 0.
            Instance.Register(CMaterialInterface::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& /*SpawnTransform*/, entt::entity DropTarget, bool /*bAttachToTarget*/) -> entt::entity
                {
                    CMaterialInterface* Material = Cast<CMaterialInterface>(Asset);
                    if (Material == nullptr || World == nullptr || DropTarget == entt::null)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (!Registry.valid(DropTarget))
                    {
                        return entt::null;
                    }

                    SStaticMeshComponent* MeshComponent = Registry.try_get<SStaticMeshComponent>(DropTarget);
                    if (MeshComponent == nullptr)
                    {
                        return entt::null;
                    }

                    if (MeshComponent->MaterialOverrides.empty())
                    {
                        MeshComponent->MaterialOverrides.resize(1);
                    }
                    MeshComponent->MaterialOverrides[0] = Material;
                    return DropTarget;
                });

            // A skeletal mesh takes the same shape as the static-mesh handler.
            Instance.Register(CSkeletalMesh::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    return DropSkeletalMesh(World, Cast<CSkeletalMesh>(Asset), SpawnTransform, DropTarget, bAttachToTarget);
                });

            // A skeleton has no geometry, so dropping it spawns the preview mesh it carries.
            Instance.Register(CSkeleton::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    CSkeleton* Skeleton = Cast<CSkeleton>(Asset);
                    if (Skeleton == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }

                    CSkeletalMesh* PreviewMesh = Skeleton->PreviewMesh.Get();
                    if (PreviewMesh == nullptr)
                    {
                        LOG_WARN("Skeleton drop: '{}' has no Preview Mesh set, so there is nothing to spawn. "
                                 "Set one on the skeleton asset, or drop a skeletal mesh instead.",
                                 Skeleton->GetName());
                        return entt::null;
                    }

                    return DropSkeletalMesh(World, PreviewMesh, SpawnTransform, DropTarget, bAttachToTarget);
                });

            // Plays immediately so the placement can be heard, and swaps rather than stacking on an emitter.
            Instance.Register(CAudioStream::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    CAudioStream* Sound = Cast<CAudioStream>(Asset);
                    if (Sound == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        if (SAudioSourceComponent* Existing = Registry.try_get<SAudioSourceComponent>(DropTarget))
                        {
                            Existing->Sound = Sound;
                            return DropTarget;
                        }
                    }

                    entt::entity Entity = World->ConstructEntity(Sound->GetName(), SpawnTransform);
                    SAudioSourceComponent& Source = Registry.emplace<SAudioSourceComponent>(Entity);
                    Source.Sound        = Sound;
                    Source.bPlayOnReady = true;
                    // Looping by default, since a one-shot that plays once on spawn reads as the drop not working.
                    Source.bLooping     = true;

                    if (bAttachToTarget && DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        ECS::Utils::ReparentEntity(Registry, Entity, DropTarget);
                    }

                    return Entity;
                });

            // The component bursts on spawn, so the effect plays as soon as it lands and can be judged.
            Instance.Register(CParticleSystem::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    CParticleSystem* System = Cast<CParticleSystem>(Asset);
                    if (System == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        if (SParticleSystemComponent* Existing = Registry.try_get<SParticleSystemComponent>(DropTarget))
                        {
                            // The extract re-reads ParticleSystem every frame, unlike the meshes that need an invalidate.
                            Existing->ParticleSystem = System;
                            Existing->Activate(/*bReset*/ true);
                            return DropTarget;
                        }
                    }

                    entt::entity Entity = World->ConstructEntity(System->GetName(), SpawnTransform);
                    SParticleSystemComponent& Component = Registry.emplace<SParticleSystemComponent>(Entity);
                    Component.ParticleSystem = System;

                    if (bAttachToTarget && DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        ECS::Utils::ReparentEntity(Registry, Entity, DropTarget);
                    }

                    return Entity;
                });

            // Lands Simulated deliberately, since an Inactive ragdoll looks identical to no ragdoll at all.
            Instance.Register(CPhysicsAsset::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    CPhysicsAsset* PhysicsAsset = Cast<CPhysicsAsset>(Asset);
                    if (PhysicsAsset == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);

                    // Attach to the drop target when it can actually wear a ragdoll.
                    entt::entity Target = entt::null;
                    if (DropTarget != entt::null && Registry.valid(DropTarget)
                        && Registry.try_get<SSkeletalMeshComponent>(DropTarget) != nullptr)
                    {
                        Target = DropTarget;
                    }

                    if (Target == entt::null)
                    {
                        CSkeleton* Skeleton = PhysicsAsset->Skeleton.Get();
                        CSkeletalMesh* PreviewMesh = (Skeleton != nullptr) ? Skeleton->PreviewMesh.Get() : nullptr;
                        if (PreviewMesh == nullptr)
                        {
                            LOG_WARN("Physics asset drop: '{}' has no skeleton preview mesh to spawn. Set a "
                                     "Preview Mesh on its skeleton, or drop it onto a skeletal mesh entity.",
                                     PhysicsAsset->GetName());
                            return entt::null;
                        }

                        Target = DropSkeletalMesh(World, PreviewMesh, SpawnTransform, DropTarget, bAttachToTarget);
                        if (Target == entt::null)
                        {
                            return entt::null;
                        }
                    }
                    else
                    {
                        // Bodies are authored against one skeleton's bone indices and would silently attach wrong.
                        CSkeletalMesh* Mesh = Registry.get<SSkeletalMeshComponent>(Target).GetSkeletalMesh();
                        if (Mesh != nullptr && Mesh->Skeleton.IsValid() && PhysicsAsset->Skeleton.IsValid()
                            && Mesh->Skeleton.Get() != PhysicsAsset->Skeleton.Get())
                        {
                            LOG_WARN("Physics asset drop: '{}' is authored for skeleton '{}' but the mesh uses '{}'.",
                                     PhysicsAsset->GetName(), PhysicsAsset->Skeleton->GetName(), Mesh->Skeleton->GetName());
                            return entt::null;
                        }
                    }

                    SRagdollComponent& Ragdoll = Registry.get_or_emplace<SRagdollComponent>(Target);
                    Ragdoll.PhysicsAsset = PhysicsAsset;
                    Ragdoll.State        = ERagdollState::Simulated;
                    return Target;
                });

            // A clip names a skeleton but not a mesh, so there is nothing sensible to spawn from one alone.
            Instance.Register(CAnimation::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& /*SpawnTransform*/, entt::entity DropTarget, bool /*bAttachToTarget*/) -> entt::entity
                {
                    CAnimation* Animation = Cast<CAnimation>(Asset);
                    if (Animation == nullptr || World == nullptr || DropTarget == entt::null)
                    {
                        return entt::null;
                    }

                    entt::registry& Registry = ECS::GetWorldRegistry(*World);
                    if (!Registry.valid(DropTarget))
                    {
                        return entt::null;
                    }

                    SSkeletalMeshComponent* MeshComponent = Registry.try_get<SSkeletalMeshComponent>(DropTarget);
                    if (MeshComponent == nullptr)
                    {
                        LOG_WARN("Animation drop: '{}' needs a skeletal mesh to play on; the target entity has none.",
                                 Animation->GetName());
                        return entt::null;
                    }

                    // Mismatched skeletons sample garbage rather than failing, so the pose breaks with no explanation.
                    CSkeletalMesh* Mesh = MeshComponent->GetSkeletalMesh();
                    if (Mesh != nullptr && Mesh->Skeleton.IsValid() && Animation->Skeleton.IsValid()
                        && Mesh->Skeleton.Get() != Animation->Skeleton.Get())
                    {
                        LOG_WARN("Animation drop: '{}' targets skeleton '{}' but the mesh uses '{}'.",
                                 Animation->GetName(), Animation->Skeleton->GetName(), Mesh->Skeleton->GetName());
                        return entt::null;
                    }

                    SSimpleAnimationComponent& AnimComponent = Registry.get_or_emplace<SSimpleAnimationComponent>(DropTarget);
                    AnimComponent.PlayAnimation(Animation, /*bLoop*/ true);
                    return DropTarget;
                });

            // A prefab instantiates at SpawnTransform under DropTarget, or as a root.
            Instance.Register(CPrefab::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    CPrefab* Prefab = Cast<CPrefab>(Asset);
                    if (Prefab == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }
                    // Instantiate takes a PARENT, so the entity under the cursor only passes on when attaching.
                    const entt::entity Parent = bAttachToTarget ? DropTarget : entt::null;
                    return Prefab->Instantiate(World, SpawnTransform, Parent);
                });
        }
        return Instance;
    }

    void FEditorAssetDropRegistry::Register(FName AssetClass, FEditorAssetDropHandler Handler)
    {
        Handlers[AssetClass] = std::move(Handler);
    }

    const FEditorAssetDropHandler* FEditorAssetDropRegistry::FindHandler(FName AssetClass) const
    {
        // Direct hit on the concrete class name first.
        auto It = Handlers.find(AssetClass);
        if (It != Handlers.end())
        {
            return &It->second;
        }

        // Walking the class chain lets a CMaterialInterface registration cover both subclasses.
        CClass* Class = FindObject<CClass>(AssetClass);
        while (Class != nullptr)
        {
            CClass* Super = Class->GetSuperClass();
            if (Super == nullptr)
            {
                break;
            }
            It = Handlers.find(Super->GetName());
            if (It != Handlers.end())
            {
                return &It->second;
            }
            Class = Super;
        }

        return nullptr;
    }
}
