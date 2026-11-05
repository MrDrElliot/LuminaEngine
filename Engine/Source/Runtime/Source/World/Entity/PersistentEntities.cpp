#include "RuntimePCH.h"
#include "PersistentEntities.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Components/DirtyComponent.h"
#include "Components/PersistentComponent.h"
#include "Components/TagComponent.h"
#include "Components/TransformComponent.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "EntityUtils.h"
#include "Scripting/EntityScript.h"
#include "UI/UIScript.h"
#include "World/World.h"
#include "World/WorldContext.h"

namespace Lumina
{
    namespace
    {
        bool HasPersistentAncestor(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            for (ECS::FEntity Parent = Hierarchy.GetParent(Entity); Parent != ECS::NullEntity; Parent = Hierarchy.GetParent(Parent))
            {
                if (Registry.HasAll<SPersistentComponent>(Parent))
                {
                    return true;
                }
            }
            return false;
        }

        // The persistent entity the level placed under the same tag, which an arrival stands in for.
        ECS::FEntity FindPlaced(CWorld* World, ECS::FEntity Arrival, const THashSet<ECS::FEntity>& Arrived)
        {
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            const FName Tag = ECS::Utils::GetEntityTag(Registry, Arrival);
            if (Tag.IsNone())
            {
                return ECS::NullEntity;
            }

            TVector<ECS::FEntity> Tagged;
            World->GetEntitiesByTag(Tag, Tagged);
            for (ECS::FEntity Candidate : Tagged)
            {
                if (!Arrived.Contains(Candidate) && Registry.HasAll<SPersistentComponent>(Candidate))
                {
                    return Candidate;
                }
            }
            return ECS::NullEntity;
        }

        void TakePlace(ECS::FRegistry& Registry, ECS::FEntity Arrival, ECS::FEntity Placed, const THashSet<ECS::FEntity>& Arrived)
        {
            if (Registry.Get<SPersistentComponent>(Arrival).bTakePlacement)
            {
                const STransformComponent* PlacedTransform = Registry.TryGet<STransformComponent>(Placed);
                STransformComponent* ArrivalTransform = Registry.TryGet<STransformComponent>(Arrival);
                if (PlacedTransform != nullptr && ArrivalTransform != nullptr)
                {
                    ArrivalTransform->SetLocalTransform(PlacedTransform->LocalTransform);
                }
                ECS::Utils::ReparentEntity(Registry, Arrival, Registry.GetHierarchy().GetParent(Placed), /*bPreserveWorld*/ false);
            }

            // The level's own references to what it placed now mean what arrived.
            const THashMap<ECS::FEntity, ECS::FEntity> Redirect = { { Placed, Arrival } };
            Registry.ForEachEntity([&](ECS::FEntity Entity)
            {
                if (!Arrived.Contains(Entity))
                {
                    ECS::Utils::RemapEntityReferences(Registry, Entity, Redirect, /*bClearUnmapped*/ false);
                }
            });
            ECS::Utils::DestroyEntityHierarchy(Registry, Placed);
        }
    }

    void FPersistentCarry::Lift(CWorld* From)
    {
        // A client gets the server's persistent entities through replication, and copies of its own would double them.
        if (From == nullptr || From->GetNetMode() == ENetMode::Client)
        {
            return;
        }

        ECS::FRegistry& Registry = From->GetEntityRegistry();
        TVector<ECS::FEntity> Carried;
        Registry.View<SPersistentComponent>().ForEach([&](ECS::FEntity Entity, const SPersistentComponent&)
        {
            if (!HasPersistentAncestor(Registry, Entity))
            {
                Carried.Add(Entity);
                Registry.GetHierarchy().ForEachDescendant(Entity, [&](ECS::FEntity Child) { Carried.Add(Child); });
            }
        });
        if (Carried.IsEmpty())
        {
            return;
        }

        // Taken before the copy, so no clone is made and the old world's teardown finds nothing to detach.
        TVector<TPair<ECS::FEntity, TVector<TStrongObjectPtr<CEntityScript>>>> Taken;
        for (ECS::FEntity Entity : Carried)
        {
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component != nullptr && !Component->Scripts.IsEmpty())
            {
                Taken.Add({ Entity, Move(Component->Scripts) });
                Component->Scripts.Reset();
            }
        }
        EntityScripts::NoteStructureChange();

        ECS::FRegistry Staging;
        THashMap<ECS::FEntity, ECS::FEntity> ToStaging;
        CPrefab::CopyRegistry(Registry, Staging, ToStaging, &Carried);

        for (auto& [Entity, Held] : Taken)
        {
            for (TStrongObjectPtr<CEntityScript>& Script : Held)
            {
                if (Script == nullptr)
                {
                    continue;
                }
                ECS::Utils::RemapEntityReferences(Script.Get(), ToStaging, /*bClearUnmapped*/ true);

                // The document belongs to the old world's UI, which goes with it.
                const bool bUIHidden = UIScripts::GetDocument(Script.Get()) != nullptr && !UIScripts::IsShown(Script.Get());
                UIScripts::Close(Script.Get());
                Scripts.Emplace(Move(Script), ToStaging[Entity], bUIHidden);
            }
        }

        FMemoryWriter Writer(Entities);
        FInProcessObjectArchiver Archive(Writer);
        ECS::Utils::SerializeRegistry(Archive, Staging);
    }

    void FPersistentCarry::Land(CWorld* To)
    {
        if (Entities.IsEmpty() || To == nullptr)
        {
            return;
        }

        ECS::FRegistry Staging;
        {
            FMemoryReader Reader(Entities);
            FInProcessObjectArchiver Archive(Reader);
            ECS::Utils::SerializeRegistry(Archive, Staging);
        }

        ECS::FRegistry& Registry = To->GetEntityRegistry();
        THashMap<ECS::FEntity, ECS::FEntity> Arrived;
        CPrefab::CopyRegistry(Staging, Registry, Arrived);

        THashSet<ECS::FEntity> ArrivedSet;
        for (const auto& [Staged, Entity] : Arrived)
        {
            ArrivedSet.Add(Entity);
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
        }

        for (FCarriedScript& Carried : Scripts)
        {
            const ECS::FEntity Entity = Arrived[Carried.Entity];
            ECS::Utils::RemapEntityReferences(Carried.Script.Get(), Arrived, /*bClearUnmapped*/ true);
            Carried.Script->SetOwner(Entity, To);
            Registry.GetOrEmplace<SEntityScriptComponent>(Entity).Scripts.Add(Carried.Script);
            Carried.Script->BindComponentRefs();
        }
        EntityScripts::NoteStructureChange();

        for (const auto& [Staged, Entity] : Arrived)
        {
            if (Registry.HasAll<SPersistentComponent>(Entity) && Registry.GetHierarchy().GetParent(Entity) == ECS::NullEntity)
            {
                if (const ECS::FEntity Placed = FindPlaced(To, Entity, ArrivedSet); Placed != ECS::NullEntity)
                {
                    TakePlace(Registry, Entity, Placed, ArrivedSet);
                }
            }
        }

        for (FCarriedScript& Carried : Scripts)
        {
            UIScripts::Open(Carried.Script.Get());
            if (Carried.bUIHidden)
            {
                UIScripts::Hide(Carried.Script.Get());
            }
            if (!Carried.Script->IsFaulted())
            {
                Carried.Script->OnTraveled();
            }
        }

        Entities.Reset();
        Scripts.Reset();
    }
}
