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
        // Shared by the skeletal-mesh, skeleton and physics-asset handlers: each resolves to a mesh and
        // then wants exactly the skeletal-mesh behavior, so the spawn lives in one place.
        // Mirrors the static-mesh handler: retarget an existing component, otherwise spawn.
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

            // Static mesh: spawn an entity carrying the mesh; if dropped on an existing
            // mesh entity, just replace its mesh asset.
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

            // Material: only meaningful when dropped on an existing mesh entity. Sets material slot 0.
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

            // Skeletal mesh: same shape as the static-mesh handler.
            Instance.Register(CSkeletalMesh::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    return DropSkeletalMesh(World, Cast<CSkeletalMesh>(Asset), SpawnTransform, DropTarget, bAttachToTarget);
                });

            // Skeleton: a skeleton is a bone hierarchy with no geometry, so there is nothing to render
            // for one directly. It carries a preview mesh for exactly this reason -- dropping the
            // skeleton spawns that, which is what someone dragging it into the world is asking for.
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

            // Audio: spawns an emitter at the drop point, playing immediately so the placement can be
            // heard while it is being positioned. Dropped onto an entity that already emits, it swaps
            // that entity's sound rather than stacking a second source on top of it.
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
                    // Looping by default: a placed world emitter is nearly always ambience, and a one-shot
                    // that plays once on spawn and never again reads as "the drop did not work".
                    Source.bLooping     = true;

                    if (bAttachToTarget && DropTarget != entt::null && Registry.valid(DropTarget))
                    {
                        ECS::Utils::ReparentEntity(Registry, Entity, DropTarget);
                    }

                    return Entity;
                });

            // Particle system: spawns an emitter at the drop point. The component defaults to emitting
            // with a burst on spawn, so the effect plays as soon as it lands and can be judged in place.
            // Dropped onto an entity that already emits, it swaps that entity's system.
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
                            // Plain assignment is enough: the render extract re-reads ParticleSystem off
                            // the component every frame rather than caching a resolve, unlike the mesh
                            // components that need an explicit invalidate.
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

            // Physics asset: gives a character a ragdoll. Dropped onto an existing skeletal mesh it just
            // attaches to that one; dropped into open space it spawns its skeleton's preview mesh first,
            // so a physics asset is draggable straight out of the browser to get something to test.
            //
            // Lands in the Simulated state deliberately: the point of dropping one in is to watch it fall
            // when the world simulates, and an Inactive ragdoll looks identical to no ragdoll at all.
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
                        // Bodies are authored against bone indices of one skeleton; against another they
                        // attach to the wrong bones rather than failing, so refuse and say which is which.
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

            // Animation: only meaningful on an entity that already has a skeletal mesh to play it on --
            // there is nothing sensible to spawn from a clip alone, since the clip names a skeleton but
            // not a mesh. Dropped on a valid target it starts looping playback immediately, which is what
            // makes dragging a clip onto a character a one-gesture preview.
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

                    // Mismatched skeletons sample garbage rather than failing, so the pose would just look
                    // broken with nothing to explain it. Refuse and say why instead.
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

            // Prefab: instantiate at SpawnTransform under DropTarget (or as a root).
            Instance.Register(CPrefab::StaticClass()->GetName(),
                [](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, entt::entity DropTarget, bool bAttachToTarget) -> entt::entity
                {
                    CPrefab* Prefab = Cast<CPrefab>(Asset);
                    if (Prefab == nullptr || World == nullptr)
                    {
                        return entt::null;
                    }
                    // Instantiate takes a PARENT, not a drop target, so the entity under the cursor is
                    // only passed on when the gesture actually meant to attach.
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

        // Walk up the reflected class chain so a registration on CMaterialInterface picks
        // up CMaterial / CMaterialInstance assets without a separate entry per subclass.
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
