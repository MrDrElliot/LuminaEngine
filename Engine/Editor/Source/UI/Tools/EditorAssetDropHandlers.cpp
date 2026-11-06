#include "EditorAssetDropHandlers.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Audio/AudioGraph.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/AssetTypes/Audio/SoundBase.h"
#include "Assets/AssetTypes/GeometryCollection/GeometryCollection.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"
#include "Assets/AssetTypes/Weather/WeatherPreset.h"
#include "Containers/Algorithm.h"
#include "Log/Log.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Scene/SceneOps.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/AnimatedSprite3DComponent.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/DestructibleComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/Sprite3DComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WeatherComponent.h"

namespace Lumina
{
    namespace
    {
        bool IsLiveEntity(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            return Entity != ECS::NullEntity && Registry.IsValid(Entity);
        }

        // Casts the dropped asset once, so each handler below is written against its own type.
        template<typename TAsset, typename TFunc>
        void RegisterTyped(FEditorAssetDropRegistry& Registry, TFunc&& Func, bool bPlacesEntity = false)
        {
            Registry.Register(TAsset::StaticClass()->GetName(),
                [Func = std::forward<TFunc>(Func)](CWorld* World, CObject* Asset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget) -> ECS::FEntity
                {
                    TAsset* Typed = Cast<TAsset>(Asset);
                    if (Typed == nullptr || World == nullptr)
                    {
                        return ECS::NullEntity;
                    }
                    return Func(World, Typed, SpawnTransform, DropTarget, bAttachToTarget);
                }, bPlacesEntity);
        }

        // Applies to the TComponent DropTarget already has, otherwise to a new one on an entity spawned at SpawnTransform.
        template<typename TComponent, typename TApply>
        ECS::FEntity RetargetOrSpawn(CWorld* World, CObject* Asset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget, TApply&& Apply)
        {
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            if (IsLiveEntity(Registry, DropTarget))
            {
                if (TComponent* Existing = Registry.TryGet<TComponent>(DropTarget))
                {
                    Apply(*Existing, /*bSpawned*/ false);
                    return DropTarget;
                }
            }

            ECS::FEntity Entity = World->ConstructEntity(Asset->GetName(), SpawnTransform);
            Apply(Registry.Emplace<TComponent>(Entity), /*bSpawned*/ true);

            if (bAttachToTarget && IsLiveEntity(Registry, DropTarget))
            {
                ECS::Utils::ReparentEntity(Registry, Entity, DropTarget);
            }
            return Entity;
        }

        ECS::FEntity DropSkeletalMesh(CWorld* World, CSkeletalMesh* Mesh, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
        {
            return RetargetOrSpawn<SSkeletalMeshComponent>(World, Mesh, SpawnTransform, DropTarget, bAttachToTarget,
                [Mesh](SSkeletalMeshComponent& Component, bool) { Component.SetSkeletalMesh(Mesh); });
        }

        // DropTarget when it wears a mesh on Skeleton, otherwise the skeleton's preview mesh spawned in its place.
        ECS::FEntity ResolveSkeletalTarget(CWorld* World, CObject* Asset, CSkeleton* Skeleton, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
        {
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            if (IsLiveEntity(Registry, DropTarget))
            {
                if (SSkeletalMeshComponent* MeshComponent = Registry.TryGet<SSkeletalMeshComponent>(DropTarget))
                {
                    // Mismatched skeletons sample garbage rather than failing, so the pose breaks with no explanation.
                    CSkeletalMesh* Mesh = MeshComponent->GetSkeletalMesh();
                    if (Mesh != nullptr && Mesh->Skeleton.IsValid() && Skeleton != nullptr && Mesh->Skeleton.Get() != Skeleton)
                    {
                        LOG_WARN("Asset drop: '{}' is authored for skeleton '{}' but the mesh uses '{}'.",
                                 Asset->GetName(), Skeleton->GetName(), Mesh->Skeleton->GetName());
                        return ECS::NullEntity;
                    }
                    return DropTarget;
                }
            }

            CSkeletalMesh* PreviewMesh = Skeleton != nullptr ? Skeleton->PreviewMesh.Get() : nullptr;
            if (PreviewMesh == nullptr)
            {
                LOG_WARN("Asset drop: '{}' has no skeleton preview mesh to spawn. Set a Preview Mesh on its skeleton, "
                         "or drop it onto a skeletal mesh entity.", Asset->GetName());
                return ECS::NullEntity;
            }
            return DropSkeletalMesh(World, PreviewMesh, SpawnTransform, DropTarget, bAttachToTarget);
        }

        struct FObjectField
        {
            CStruct*   Component = nullptr;
            void*      Instance  = nullptr;
            FProperty* Property  = nullptr;

            TStrongObjectPtr<CObject>* Slot() const { return Property->GetValuePtr<TStrongObjectPtr<CObject>>(Instance); }
        };

        void RegisterHandlers(FEditorAssetDropRegistry& Instance)
        {
            RegisterTyped<CStaticMesh>(Instance, [](CWorld* World, CStaticMesh* Mesh, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                bool bSpawned = false;
                const ECS::FEntity Entity = RetargetOrSpawn<SStaticMeshComponent>(World, Mesh, SpawnTransform, DropTarget, bAttachToTarget,
                    [Mesh, &bSpawned](SStaticMeshComponent& Component, bool bNew) { Component.SetStaticMesh(Mesh); bSpawned = bNew; });

                if (bSpawned)
                {
                    SceneOps::AddPlacementCollision(World->GetEntityRegistry(), Entity);
                }
                return Entity;
            }, /*bPlacesEntity*/ true);

            // A material is only meaningful on an existing mesh entity, and sets slot 0.
            RegisterTyped<CMaterialInterface>(Instance, [](CWorld* World, CMaterialInterface* Material, const FTransform&, ECS::FEntity DropTarget, bool)
            {
                ECS::FRegistry& Registry = World->GetEntityRegistry();
                SStaticMeshComponent* MeshComponent = IsLiveEntity(Registry, DropTarget) ? Registry.TryGet<SStaticMeshComponent>(DropTarget) : nullptr;
                if (MeshComponent == nullptr)
                {
                    return ECS::NullEntity;
                }

                if (MeshComponent->MaterialOverrides.IsEmpty())
                {
                    MeshComponent->MaterialOverrides.SetNum(1);
                }
                MeshComponent->MaterialOverrides[0] = Material;
                MeshComponent->MarkRenderStateDirty();
                return DropTarget;
            });

            RegisterTyped<CSkeletalMesh>(Instance, &DropSkeletalMesh, /*bPlacesEntity*/ true);

            // A skeleton has no geometry, so dropping it spawns the preview mesh it carries.
            RegisterTyped<CSkeleton>(Instance, [](CWorld* World, CSkeleton* Skeleton, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                CSkeletalMesh* PreviewMesh = Skeleton->PreviewMesh.Get();
                if (PreviewMesh == nullptr)
                {
                    LOG_WARN("Skeleton drop: '{}' has no Preview Mesh set, so there is nothing to spawn. "
                             "Set one on the skeleton asset, or drop a skeletal mesh instead.", Skeleton->GetName());
                    return ECS::NullEntity;
                }
                return DropSkeletalMesh(World, PreviewMesh, SpawnTransform, DropTarget, bAttachToTarget);
            }, /*bPlacesEntity*/ true);

            // Plays immediately so the placement can be heard, and swaps rather than stacking on an emitter.
            auto DropSound = [](CWorld* World, CSoundBase* Sound, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                return RetargetOrSpawn<SAudioSourceComponent>(World, Sound, SpawnTransform, DropTarget, bAttachToTarget,
                    [Sound](SAudioSourceComponent& Source, bool bSpawned)
                    {
                        Source.Sound = Sound;
                        if (bSpawned)
                        {
                            Source.bPlayOnReady = true;
                            // Looping by default, since a one-shot that plays once on spawn reads as the drop not working.
                            Source.bLooping = true;
                        }
                    });
            };
            RegisterTyped<CAudioStream>(Instance, DropSound, /*bPlacesEntity*/ true);
            RegisterTyped<CAudioGraph>(Instance, DropSound, /*bPlacesEntity*/ true);

            // The component bursts on spawn, so the effect plays as soon as it lands and can be judged.
            RegisterTyped<CParticleSystem>(Instance, [](CWorld* World, CParticleSystem* System, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                return RetargetOrSpawn<SParticleSystemComponent>(World, System, SpawnTransform, DropTarget, bAttachToTarget,
                    [System](SParticleSystemComponent& Component, bool bSpawned)
                    {
                        // The extract re-reads ParticleSystem every frame, unlike the meshes that need an invalidate.
                        Component.ParticleSystem = System;
                        if (!bSpawned)
                        {
                            Component.Activate(/*bReset*/ true);
                        }
                    });
            }, /*bPlacesEntity*/ true);

            // Lands Simulated deliberately, since an Inactive ragdoll looks identical to no ragdoll at all.
            RegisterTyped<CPhysicsAsset>(Instance, [](CWorld* World, CPhysicsAsset* PhysicsAsset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                const ECS::FEntity Target = ResolveSkeletalTarget(World, PhysicsAsset, PhysicsAsset->Skeleton.Get(), SpawnTransform, DropTarget, bAttachToTarget);
                if (Target == ECS::NullEntity)
                {
                    return ECS::NullEntity;
                }

                SRagdollComponent& Ragdoll = World->GetEntityRegistry().GetOrEmplace<SRagdollComponent>(Target);
                Ragdoll.PhysicsAsset = PhysicsAsset;
                Ragdoll.State        = ERagdollState::Simulated;
                return Target;
            });

            RegisterTyped<CAnimation>(Instance, [](CWorld* World, CAnimation* Animation, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                const ECS::FEntity Target = ResolveSkeletalTarget(World, Animation, Animation->Skeleton.Get(), SpawnTransform, DropTarget, bAttachToTarget);
                if (Target == ECS::NullEntity)
                {
                    return ECS::NullEntity;
                }

                World->GetEntityRegistry().GetOrEmplace<SSimpleAnimationComponent>(Target).PlayAnimation(Animation, /*bLoop*/ true);
                return Target;
            });

            RegisterTyped<CAnimationGraph>(Instance, [](CWorld* World, CAnimationGraph* Graph, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                const ECS::FEntity Target = ResolveSkeletalTarget(World, Graph, Graph->Skeleton.Get(), SpawnTransform, DropTarget, bAttachToTarget);
                if (Target == ECS::NullEntity)
                {
                    return ECS::NullEntity;
                }

                World->GetEntityRegistry().GetOrEmplace<SAnimationGraphComponent>(Target).Graph = Graph;
                return Target;
            });

            // A prefab instantiates at SpawnTransform under DropTarget, or as a root.
            RegisterTyped<CPrefab>(Instance, [](CWorld* World, CPrefab* Prefab, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                // Instantiate takes a parent, so the entity under the cursor only passes on when attaching.
                const ECS::FEntity Parent = bAttachToTarget ? DropTarget : ECS::NullEntity;
                return Prefab->Instantiate(World, SpawnTransform, Parent);
            }, /*bPlacesEntity*/ true);

            // A destructible needs the mesh it breaks from, so a collection on its own spawns its source mesh.
            RegisterTyped<CGeometryCollection>(Instance, [](CWorld* World, CGeometryCollection* Collection, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                ECS::FRegistry& Registry = World->GetEntityRegistry();
                ECS::FEntity Target = DropTarget;
                if (!IsLiveEntity(Registry, Target) || Registry.TryGet<SStaticMeshComponent>(Target) == nullptr)
                {
                    CStaticMesh* SourceMesh = Collection->SourceMesh.Get();
                    if (SourceMesh == nullptr)
                    {
                        LOG_WARN("Geometry collection drop: '{}' has no source mesh to spawn. Drop it onto a static mesh entity.",
                                 Collection->GetName());
                        return ECS::NullEntity;
                    }
                    Target = RetargetOrSpawn<SStaticMeshComponent>(World, Collection, SpawnTransform, ECS::NullEntity, false,
                        [SourceMesh](SStaticMeshComponent& Component, bool) { Component.SetStaticMesh(SourceMesh); });
                    if (bAttachToTarget && IsLiveEntity(Registry, DropTarget))
                    {
                        ECS::Utils::ReparentEntity(Registry, Target, DropTarget);
                    }
                }

                Registry.GetOrEmplace<SDestructibleComponent>(Target).Collection = Collection;
                return Target;
            });

            RegisterTyped<CSequence>(Instance, [](CWorld* World, CSequence* Sequence, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                return RetargetOrSpawn<SSequencePlayerComponent>(World, Sequence, SpawnTransform, DropTarget, bAttachToTarget,
                    [Sequence](SSequencePlayerComponent& Player, bool) { Player.Sequence = Sequence; });
            });

            // Weather is one per world, so a drop swaps the preset on the existing component wherever it sits.
            RegisterTyped<CWeatherPreset>(Instance, [](CWorld* World, CWeatherPreset* Preset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                ECS::FRegistry& Registry = World->GetEntityRegistry();
                ECS::FEntity Existing = ECS::NullEntity;
                for (ECS::FEntity Entity : Registry.View<SWeatherComponent>())
                {
                    Existing = Entity;
                    break;
                }

                return RetargetOrSpawn<SWeatherComponent>(World, Preset, SpawnTransform, Existing != ECS::NullEntity ? Existing : DropTarget, bAttachToTarget,
                    [Preset](SWeatherComponent& Weather, bool) { Weather.Preset = Preset; });
            });

            // The animated sprite only drives playback, so a spawned one gets the sprite that draws it.
            RegisterTyped<CSpriteSheet>(Instance, [](CWorld* World, CSpriteSheet* Sheet, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)
            {
                const ECS::FEntity Target = RetargetOrSpawn<SAnimatedSprite3DComponent>(World, Sheet, SpawnTransform, DropTarget, bAttachToTarget,
                    [Sheet](SAnimatedSprite3DComponent& Animated, bool) { Animated.SpriteSheet = Sheet; });

                // Seeded from the sheet so it draws in the editor, where the animation system does not run.
                SSprite3DComponent& Sprite = World->GetEntityRegistry().GetOrEmplace<SSprite3DComponent>(Target);
                Sprite.Texture = Sheet->Texture;
                Sprite.HFrames = Math::Max(Sheet->HFrames, 1);
                Sprite.VFrames = Math::Max(Sheet->VFrames, 1);
                return Target;
            }, /*bPlacesEntity*/ true);
        }
    }

    FEditorAssetDropRegistry& FEditorAssetDropRegistry::Get()
    {
        static FEditorAssetDropRegistry Instance;
        static bool bRegistered = false;
        if (!bRegistered)
        {
            bRegistered = true;
            RegisterHandlers(Instance);
        }
        return Instance;
    }

    void FEditorAssetDropRegistry::Register(FName AssetClass, FEditorAssetDropHandler Handler, bool bPlacesEntity)
    {
        Handlers[AssetClass] = FEntry{ Move(Handler), bPlacesEntity };
    }

    const FEditorAssetDropHandler* FEditorAssetDropRegistry::FindHandler(FName AssetClass) const
    {
        const FEntry* Entry = FindEntry(AssetClass);
        return Entry != nullptr ? &Entry->Handler : nullptr;
    }

    bool FEditorAssetDropRegistry::PlacesEntity(FName AssetClass) const
    {
        const FEntry* Entry = FindEntry(AssetClass);
        return Entry != nullptr && Entry->bPlacesEntity;
    }

    const FEditorAssetDropRegistry::FEntry* FEditorAssetDropRegistry::FindEntry(FName AssetClass) const
    {
        // Direct hit on the concrete class name first.
        auto It = Handlers.Find(AssetClass);
        if (It != nullptr)
        {
            return &(*It);
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
            It = Handlers.Find(Super->GetName());
            if (It != nullptr)
            {
                return &(*It);
            }
            Class = Super;
        }

        return nullptr;
    }

    ECS::FEntity FEditorAssetDropRegistry::AssignToComponentField(CWorld* World, CObject* Asset, ECS::FEntity DropTarget)
    {
        if (World == nullptr || Asset == nullptr)
        {
            return ECS::NullEntity;
        }

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        if (!IsLiveEntity(Registry, DropTarget))
        {
            return ECS::NullEntity;
        }

        TVector<FObjectField> Fields;
        ECS::Utils::ForEachComponent(Registry, DropTarget, [&](void* Component, ECS::FSparseSet&, CStruct* Struct)
        {
            for (FProperty* Property : Struct->GetProperties())
            {
                if (!Property->IsA(EPropertyTypeFlags::Object) || !Property->IsVisible() || Property->IsReadOnly())
                {
                    continue;
                }
                CClass* Accepts = static_cast<FObjectProperty*>(Property)->GetPropertyClass();
                if (Accepts != nullptr && Asset->IsA(Accepts))
                {
                    Fields.Emplace(Struct, Component, Property);
                }
            }
        });

        if (Fields.IsEmpty())
        {
            LOG_WARN("Asset drop: nothing on the target takes a '{}'.", Asset->GetClass()->GetName());
            return ECS::NullEntity;
        }

        // With several fields that fit, an empty one is filled rather than guessing which set one to replace.
        auto Chosen = Fields.begin();
        if (Fields.Num() > 1)
        {
            Chosen = Algo::FindIf(Fields, [](const FObjectField& Field) { return Field.Slot()->Get() == nullptr; });
            if (Chosen == Fields.end())
            {
                LOG_WARN("Asset drop: {} fields on the target take a '{}' and all are set, so set the one you mean in the details panel.",
                         Fields.Num(), Asset->GetClass()->GetName());
                return ECS::NullEntity;
            }
        }

        {
            SceneOps::FPropertyEditScope Edit(Registry, DropTarget, Chosen->Component, Chosen->Instance, Chosen->Property, Chosen->Slot());
            *Chosen->Slot() = Asset;
        }
        if (const FComponentOps* Ops = Chosen->Component->GetComponentOps())
        {
            Ops->Patch(Registry, DropTarget);
        }
        return DropTarget;
    }

    ECS::FEntity FEditorAssetDropRegistry::Drop(CWorld* World, CObject* Asset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget) const
    {
        if (World == nullptr || Asset == nullptr)
        {
            return ECS::NullEntity;
        }

        const FEditorAssetDropHandler* Handler = FindHandler(Asset->GetClass()->GetName());
        if (Handler != nullptr && *Handler)
        {
            return (*Handler)(World, Asset, SpawnTransform, DropTarget, bAttachToTarget);
        }
        return AssignToComponentField(World, Asset, DropTarget);
    }
}
