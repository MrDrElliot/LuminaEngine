#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "World/ECS/Registry.h"
#include "Prefab.h"
#include "PrefabComponents.h"
#include "PrefabOverride.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "GUID/GUID.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        /** Component types skipped by the cross-registry copy pass. */
        bool IsNonReplicatedStorage(uint32 ID)
        {
            // Relationships are remapped manually after the copy pass.
            if (ID == ECS::GetComponentTypeID<FRelationshipComponent>()) return true;
            // Editor-only state must not leak between worlds and prefabs.
            if (ID == ECS::GetComponentTypeID<FSelectedInEditorComponent>()) return true;
            if (ID == ECS::GetComponentTypeID<FHideInSceneOutliner>()) return true;
            if (ID == ECS::GetComponentTypeID<FEditorComponent>()) return true;
            return false;
        }

        // Runtime-only / spawn-tagging components a refresh diff must never remove; they'd re-spawn next frame.
        bool IsRuntimeOnlyComponent(uint32 ID)
        {
            if (ID == ECS::GetComponentTypeID<SPrefabInstanceComponent>()) return true;
            // The ledger lives on the instance root and is absent from the prefab, so it must survive every refresh.
            if (ID == ECS::GetComponentTypeID<SPrefabOverrideComponent>()) return true;
            if (ID == ECS::GetComponentTypeID<SRigidBodyComponent>())      return true;
            if (ID == ECS::GetComponentTypeID<FNeedsTransformUpdate>())    return true;
            if (ID == ECS::GetComponentTypeID<FNeedsPhysicsBodyUpdate>())  return true;
            return false;
        }

        FName GenerateStableID()
        {
            return FName(FGuid::New().ToShortString());
        }

        // Game thread only, since every writer runs from world init, the prefab editor or asset capture.
        uint32 GDataGeneration = 0;

        // Nested instance tracking must not leak into the new prefab, which gets fresh tags instead.
        bool ShouldSkipInstanceComponent(uint32 ID)
        {
            return CPrefab::IsInstanceTrackingComponent(ID);
        }
        
        void* FindReflectedComponentPtr(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* Struct)
        {
            if (Struct == nullptr || !Registry.IsValid(Entity))
            {
                return nullptr;
            }
            const FComponentOps* Ops = Struct->GetComponentOps();
            if (Ops == nullptr)
            {
                return nullptr;
            }
            if (auto* Storage = Registry.FindStorage(static_cast<uint32>(Ops->TypeId)))
            {
                if (Storage->Contains(Entity))
                {
                    return Storage->GetRaw(Entity);
                }
            }
            return nullptr;
        }

        // Otherwise freshly spawned entities render at a stale position for one frame.
        void MarkSubtreeTransformsDirty(ECS::FRegistry& Registry, ECS::FEntity Root)
        {
            if (!Registry.IsValid(Root))
            {
                return;
            }
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Root);
            ECS::Utils::ForEachDescendant(Registry, Root, [&](ECS::FEntity Desc)
            {
                Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Desc);
            });
        }
    }

    bool CPrefab::IsInstanceTrackingComponent(uint32 ID)
    {
        return ID == ECS::GetComponentTypeID<SPrefabInstanceComponent>()
            || ID == ECS::GetComponentTypeID<SPrefabOverrideComponent>();
    }

    uint32 CPrefab::GetDataGeneration()
    {
        return GDataGeneration;
    }

    void CPrefab::BumpDataGeneration()
    {
        ++GDataGeneration;
    }

    void CPrefab::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Prefabs");
        CObject::Serialize(Ar);

        // Resolved data would go stale the moment the parent changed, which a variant must never do.
        bool bVariantPayload = HoldsVariantPayload();
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::PREFAB_VARIANTS)
        {
            Ar << bVariantPayload;
        }
        else
        {
            bVariantPayload = false;
        }

        if (Ar.IsReading())
        {
            bVariantPayloadOnDisk = bVariantPayload;
        }

        ECS::Utils::SerializeRegistry(Ar, bVariantPayload ? VariantDelta : Registry);
    }

    void CPrefab::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Prefabs");
        CObject::PostLoad();

        // A dependency's DATA is not loaded at serialize time, and by PostLoad the whole set is in.
        if (IsVariant())
        {
            ResolveVariant();
        }
        else if (bVariantPayloadOnDisk)
        {
            // Registry holds nothing to resolve onto, and unflagged that reads as a prefab shipping no entities.
            LOG_ERROR("Prefab '{}' is a variant whose parent could not be loaded; leaving it unresolved.",
                GetName().c_str());
            bVariantResolveFailed = true;
        }
    }

    void CPrefab::CopyRegistry(ECS::FRegistry& Source, ECS::FRegistry& Dest, THashMap<ECS::FEntity, ECS::FEntity>& OutMap,
        const TVector<ECS::FEntity>* SourceEntities, bool(*ExtraSkipStorage)(uint32))
    {

        if (SourceEntities != nullptr)
        {
            for (ECS::FEntity SrcE : *SourceEntities)
            {
                if (Source.IsValid(SrcE))
                {
                    OutMap[SrcE] = Dest.Create();
                }
            }
        }
        else
        {
            Source.ForEachEntity([&](ECS::FEntity SrcE)
            {
                OutMap[SrcE] = Dest.Create();
            });
        }

        for (Lumina::ECS::FSparseSet* SrcSetPtr : Source.GetActiveStorages())
        {
            const Lumina::ECS::FComponentTypeID ID = SrcSetPtr->GetTypeInfo().TypeID;
            Lumina::ECS::FSparseSet& SrcSet = *SrcSetPtr;
            // Rigid bodies carry a runtime BodyID that must not be copied; handled below.
            if (IsNonReplicatedStorage(ID)
                || ID == ECS::GetComponentTypeID<SRigidBodyComponent>()
                || (ExtraSkipStorage != nullptr && ExtraSkipStorage(ID)))
            {
                continue;
            }

            CStruct* CompStruct = FindComponentStructByTypeId(ID);
            if (CompStruct == nullptr)
            {
                continue;
            }
            const FComponentOps* Ops = CompStruct->GetComponentOps();

            for (ECS::FEntity SrcE : SrcSet)
            {
                auto It = OutMap.find(SrcE);
                if (It == OutMap.end())
                {
                    continue;
                }

                ECS::FEntity DestE = It->second;
                void* SrcCompPtr = SrcSet.GetRaw(SrcE);

                Ops->EmplaceCopy(Dest, DestE, SrcCompPtr);
            }
        }

        // Copying the live body id makes the physics scene skip creation as already existing.
        for (auto& [SrcE, DestE] : OutMap)
        {
            if (const SRigidBodyComponent* SrcBody = Source.TryGet<SRigidBodyComponent>(SrcE))
            {
                SRigidBodyComponent NewBody = *SrcBody;
                NewBody.BodyID = 0xFFFFFFFFu;
                Dest.EmplaceOrReplace<SRigidBodyComponent>(DestE, NewBody);
            }
        }

        auto Remap = [&](ECS::FEntity& E)
        {
            if (E != ECS::NullEntity)
            {
                auto It = OutMap.find(E);
                E = (It != OutMap.end()) ? It->second : ECS::NullEntity;
            }
        };

        Source.View<FRelationshipComponent>().ForEach([&](ECS::FEntity SrcE, const FRelationshipComponent& SrcRel)
        {
            auto It = OutMap.find(SrcE);
            if (It == OutMap.end())
            {
                return;
            }

            FRelationshipComponent DestRel = SrcRel;
            Remap(DestRel.First);
            Remap(DestRel.Prev);
            Remap(DestRel.Next);
            Remap(DestRel.Parent);
            Dest.EmplaceOrReplace<FRelationshipComponent>(It->second, DestRel);
        });

        // References escaping the copied set are cleared, so a stale id cannot alias an unrelated entity.
        for (auto& [SrcE, DestE] : OutMap)
        {
            ECS::Utils::RemapEntityReferences(Dest, DestE, OutMap, /*bClearUnmapped*/ true);
        }
        
        for (auto& [SrcE, DestE] : OutMap)
        {
            if (STransformComponent* DestTransform = Dest.TryGet<STransformComponent>(DestE))
            {
                DestTransform->Bind(Dest, DestE);
                DestTransform->ResetDirtyState();
            }
        }
    }

    ECS::FEntity CPrefab::Instantiate(CWorld* TargetWorld, const FTransform& OffsetTransform, ECS::FEntity Parent)
    {
        if (TargetWorld == nullptr)
        {
            return ECS::NullEntity;
        }

        if (bVariantResolveFailed)
        {
            LOG_WARN("Prefab '{}' did not resolve against its parent; refusing to instantiate it.", GetName().c_str());
            return ECS::NullEntity;
        }

        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*TargetWorld);
        
        TVector<ECS::FEntity> PrefabRoots;
        PrefabRoots.reserve(2);
        Registry.ForEachEntity([&](ECS::FEntity E)
        {
            const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(E);
            const bool bHasParent = Rel && Rel->Parent != ECS::NullEntity;
            if (!bHasParent)
            {
                PrefabRoots.push_back(E);
            }
        });

        if (PrefabRoots.empty())
        {
            LOG_WARN("Prefab '{}' has no entities; nothing to instantiate.", GetName().c_str());
            return ECS::NullEntity;
        }

        const ECS::FEntity PrefabRoot = PrefabRoots[0];
        if (PrefabRoots.size() > 1)
        {
            LOG_WARN("Prefab '{}' has {} parentless entities; reparenting extras under the first.",
                     GetName().c_str(), (uint32)PrefabRoots.size());
        }
        
        struct FBodyBatchScope
        {
            Physics::IPhysicsScene* Scene;
            explicit FBodyBatchScope(Physics::IPhysicsScene* InScene) : Scene(InScene)
            {
                if (Scene) { Scene->BeginBodyBatch(); }
            }
            ~FBodyBatchScope() { if (Scene) { Scene->EndBodyBatch(); } }
        };

        THashMap<ECS::FEntity, ECS::FEntity> Map;
        ECS::FEntity WorldRoot = ECS::NullEntity;

        {
            FBodyBatchScope BodyBatch(TargetWorld->GetPhysicsScene());

            CopyRegistry(Registry, WorldRegistry, Map);

            WorldRoot = Map[PrefabRoot];

            for (auto& [SrcE, DestE] : Map)
            {
                FName StableID;
                if (const SPrefabComponent* PrefabComp = WorldRegistry.TryGet<SPrefabComponent>(DestE))
                {
                    StableID = PrefabComp->StableID;
                }
                else
                {
                    StableID = GenerateStableID();
                }

                WorldRegistry.Remove<SPrefabComponent>(DestE);

                SPrefabInstanceComponent& Instance = WorldRegistry.EmplaceOrReplace<SPrefabInstanceComponent>(DestE);
                Instance.SourcePrefab = this;
                Instance.StableID = StableID;
                Instance.bIsRoot = (DestE == WorldRoot);
            }

            // Rescue any extra parentless entities so the spawn has a single hierarchical root.
            for (size_t i = 1; i < PrefabRoots.size(); ++i)
            {
                const ECS::FEntity Extra = Map[PrefabRoots[i]];
                if (Extra != ECS::NullEntity && WorldRegistry.IsValid(Extra))
                {
                    ECS::Utils::ReparentEntity(WorldRegistry, Extra, WorldRoot);
                }
            }

            if (STransformComponent* RootTransform = WorldRegistry.TryGet<STransformComponent>(WorldRoot))
            {
                RootTransform->SetLocalTransform(OffsetTransform);
            }
            else
            {
                WorldRegistry.Emplace<STransformComponent>(WorldRoot, OffsetTransform);
            }

            if (Parent != ECS::NullEntity && WorldRegistry.IsValid(Parent))
            {
                ECS::Utils::ReparentEntity(WorldRegistry, WorldRoot, Parent);
            }
            
            MarkSubtreeTransformsDirty(WorldRegistry, WorldRoot);
        }

        return WorldRoot;
    }

    void CPrefab::RefreshInstance(CWorld* World, ECS::FEntity InstanceRoot)
    {

        if (World == nullptr)
        {
            return;
        }

        // An empty Registry here would read as the prefab having deleted every entity it ships.
        if (bVariantResolveFailed)
        {
            LOG_WARN("Prefab '{}' did not resolve against its parent; leaving its instances untouched.",
                GetName().c_str());
            return;
        }

        // Same hazard from any other cause, and a refusal costs a stale instance where a diff costs the placement.
        if (Registry.NumEntities() == 0)
        {
            LOG_WARN("Prefab '{}' has no entities; leaving its instances untouched rather than emptying them.",
                GetName().c_str());
            return;
        }

        ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*World);
        if (!WorldRegistry.IsValid(InstanceRoot))
        {
            return;
        }

        SPrefabInstanceComponent* RootInstance = WorldRegistry.TryGet<SPrefabInstanceComponent>(InstanceRoot);
        if (RootInstance == nullptr || RootInstance->SourcePrefab != this)
        {
            return;
        }

        // Everything past here rewrites this instance's components, invalidating any cached pointer into them.
        BumpDataGeneration();

        // Index instance entities by StableID.
        THashMap<FName, ECS::FEntity> InstanceByStableID;
        InstanceByStableID[RootInstance->StableID] = InstanceRoot;
        ECS::Utils::ForEachDescendant(WorldRegistry, InstanceRoot, [&](ECS::FEntity Descendant)
        {
            if (const SPrefabInstanceComponent* Inst = WorldRegistry.TryGet<SPrefabInstanceComponent>(Descendant))
            {
                if (Inst->SourcePrefab == this && !Inst->StableID.IsNone())
                {
                    InstanceByStableID[Inst->StableID] = Descendant;
                }
            }
        });

        // Index prefab entities by StableID.
        THashMap<FName, ECS::FEntity> PrefabByStableID;
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity PrefabE, const SPrefabComponent& PrefabComp)
        {
            if (!PrefabComp.StableID.IsNone())
            {
                PrefabByStableID[PrefabComp.StableID] = PrefabE;
            }
        });

        // Destroy instance entities whose prefab counterpart is gone (never the user-placed root).
        TVector<ECS::FEntity> ToDestroy;
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (WorldE == InstanceRoot)
            {
                continue;
            }
            if (PrefabByStableID.find(StableID) == PrefabByStableID.end())
            {
                ToDestroy.push_back(WorldE);
            }
        }
        // Survivors detach to the root first, and the hierarchy-mirror pass re-nests them per the prefab.
        THashSet<ECS::FEntity> DeadSet;
        DeadSet.reserve(ToDestroy.size());
        for (ECS::FEntity E : ToDestroy)
        {
            DeadSet.insert(E);
        }
        for (ECS::FEntity Dead : ToDestroy)
        {
            if (!WorldRegistry.IsValid(Dead))
            {
                continue;
            }
            TVector<ECS::FEntity> Survivors;
            ECS::Utils::ForEachDescendant(WorldRegistry, Dead, [&](ECS::FEntity Desc)
            {
                if (DeadSet.find(Desc) == DeadSet.end())
                {
                    Survivors.push_back(Desc);
                }
            });
            for (ECS::FEntity S : Survivors)
            {
                if (WorldRegistry.IsValid(S) && WorldRegistry.HasAll<STransformComponent>(S))
                {
                    ECS::Utils::ReparentEntity(WorldRegistry, S, InstanceRoot);
                }
            }
        }

        for (ECS::FEntity E : ToDestroy)
        {
            if (WorldRegistry.IsValid(E))
            {
                ECS::Utils::DestroyEntityHierarchy(WorldRegistry, E);
            }
        }

        // A node taken down with its parent leaves an entry that would remap a handle onto a dead entity.
        TVector<FName> StaleIDs;
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (!WorldRegistry.IsValid(WorldE))
            {
                StaleIDs.push_back(StableID);
            }
        }
        for (const FName& StaleID : StaleIDs)
        {
            InstanceByStableID.erase(StaleID);
        }

        // Spawn instance entities for new prefab entries.
        for (auto& [StableID, PrefabE] : PrefabByStableID)
        {
            if (InstanceByStableID.find(StableID) != InstanceByStableID.end())
            {
                continue;
            }
            const ECS::FEntity NewE = WorldRegistry.Create();
            InstanceByStableID[StableID] = NewE;

            SPrefabInstanceComponent& Inst = WorldRegistry.Emplace<SPrefabInstanceComponent>(NewE);
            Inst.SourcePrefab = this;
            Inst.StableID     = StableID;
            Inst.bIsRoot      = false;
        }

        // Prefab-entity -> instance-entity remap table (for entity-handle fields).
        THashMap<ECS::FEntity, ECS::FEntity> PrefabToInstance;
        for (auto& [StableID, PrefabE] : PrefabByStableID)
        {
            auto It = InstanceByStableID.find(StableID);
            if (It != InstanceByStableID.end())
            {
                PrefabToInstance[PrefabE] = It->second;
            }
        }

        // Absent means nothing overridden, so every inherited component refreshes wholesale.
        THashMap<FName, THashMap<FName, THashSet<FName>>> OverriddenLeaves; // node StableID -> comp name -> leaf paths
        THashMap<FName, THashSet<FName>> AddedComponents;                   // node StableID -> comp names
        THashMap<FName, THashSet<FName>> RemovedComponents;                 // node StableID -> comp names
        if (const SPrefabOverrideComponent* Ledger = WorldRegistry.TryGet<SPrefabOverrideComponent>(InstanceRoot))
        {
            for (const SPrefabPropertyOverride& O : Ledger->PropertyOverrides)
            {
                OverriddenLeaves[O.EntityStableID][O.ComponentType].insert(O.PropertyPath);
            }
            for (const SPrefabComponentRef& C : Ledger->AddedComponents)
            {
                AddedComponents[C.EntityStableID].insert(C.ComponentType);
            }
            for (const SPrefabComponentRef& C : Ledger->RemovedComponents)
            {
                RemovedComponents[C.EntityStableID].insert(C.ComponentType);
            }
        }

        // This subsumes the old root-only case and keeps gizmo edits, which bypass the property hook.
        const uint32 TransformID = ECS::GetComponentTypeID<STransformComponent>();
        const uint32 ScriptComponentID = ECS::GetComponentTypeID<SEntityScriptComponent>();

        // EmplaceOrReplace raises OnUpdate rather than OnDestroy, so a script's OnDetach is skipped.
        auto EmplaceFromPrefab = [&](uint32 ID, const FComponentOps* Ops, ECS::FEntity WorldE, void* SrcCompPtr)
        {
            if (ID == ScriptComponentID)
            {
                EntityScripts::DetachAll(WorldRegistry, WorldE);
            }
            Ops->EmplaceCopy(WorldRegistry, WorldE, SrcCompPtr);
        };

        // Copy/replace components prefab -> instance honoring overrides, then prune ones the prefab dropped.
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity PrefabE, const SPrefabComponent& PrefabComp)
        {
            auto It = InstanceByStableID.find(PrefabComp.StableID);
            if (It == InstanceByStableID.end())
            {
                return;
            }

            const ECS::FEntity WorldE = It->second;
            if (!WorldRegistry.IsValid(WorldE))
            {
                return; // stale mapping (e.g. collaterally destroyed); skip this node.
            }
            const FName NodeID = PrefabComp.StableID;

            const THashMap<FName, THashSet<FName>>* NodeOverrides = nullptr;
            if (auto NIt = OverriddenLeaves.find(NodeID); NIt != OverriddenLeaves.end())
            {
                NodeOverrides = &NIt->second;
            }
            const THashSet<FName>* NodeRemoved = nullptr;
            if (auto RIt = RemovedComponents.find(NodeID); RIt != RemovedComponents.end())
            {
                NodeRemoved = &RIt->second;
            }
            const THashSet<FName>* NodeAdded = nullptr;
            if (auto AIt = AddedComponents.find(NodeID); AIt != AddedComponents.end())
            {
                NodeAdded = &AIt->second;
            }
            const bool bNodeHasLedger = (NodeOverrides != nullptr) || (NodeRemoved != nullptr);

            bool bEntityHasOverrides = false;

            // Track the prefab's storages for this entity so we can drop ones the prefab no longer has.
            THashSet<uint32> PrefabComponentIDs;
            PrefabComponentIDs.reserve(8);

            for (Lumina::ECS::FSparseSet* PrefabStoragePtr : Registry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = PrefabStoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& PrefabStorage = *PrefabStoragePtr;
                if (IsNonReplicatedStorage(ID)) continue;
                if (ID == ECS::GetComponentTypeID<SPrefabComponent>()) continue;
                if (!PrefabStorage.Contains(PrefabE)) continue;

                CStruct* CompStruct = FindComponentStructByTypeId(ID);
                if (CompStruct == nullptr) continue;

                const FComponentOps* Ops = CompStruct->GetComponentOps();
                void* SrcCompPtr = PrefabStorage.GetRaw(PrefabE);

                // A spawned entity left with no transform would crash the hierarchy-mirror reparent.
                if (ID == TransformID)
                {
                    auto* WorldXform = WorldRegistry.FindStorage(ID);
                    if (WorldXform == nullptr || !WorldXform->Contains(WorldE))
                    {
                        Ops->EmplaceCopy(WorldRegistry, WorldE, SrcCompPtr);
                    }
                    continue;
                }

                // A node with no ledger refreshes exactly as before.
                if (!bNodeHasLedger)
                {
                    PrefabComponentIDs.insert(ID);
                    EmplaceFromPrefab(ID, Ops, WorldE, SrcCompPtr);
                    continue;
                }

                const FName CompName = CompStruct->GetName();

                // The instance deleted this inherited component, so leave it absent rather than re-adding.
                if (NodeRemoved && !CompName.IsNone() && NodeRemoved->find(CompName) != NodeRemoved->end())
                {
                    continue;
                }

                PrefabComponentIDs.insert(ID);

                const THashSet<FName>* CompOverrides = nullptr;
                if (NodeOverrides && CompStruct)
                {
                    if (auto OIt = NodeOverrides->find(CompName); OIt != NodeOverrides->end() && !OIt->second.empty())
                    {
                        CompOverrides = &OIt->second;
                    }
                }

                void* DstCompPtr = nullptr;
                if (auto* WorldStorage = WorldRegistry.FindStorage(ID))
                {
                    if (WorldStorage->Contains(WorldE))
                    {
                        DstCompPtr = WorldStorage->GetRaw(WorldE);
                    }
                }

                // Otherwise replace wholesale, which also adds a missing inherited component.
                if (CompOverrides != nullptr && DstCompPtr != nullptr)
                {
                    PrefabOverride::ApplyInheritedLeaves(CompStruct, DstCompPtr, SrcCompPtr, *CompOverrides);
                    bEntityHasOverrides = true;
                }
                else
                {
                    EmplaceFromPrefab(ID, Ops, WorldE, SrcCompPtr);
                }
            }

            // Skips non-replicated, transform and instance-added components.
            TVector<uint32> ToRemoveStorages;
            for (Lumina::ECS::FSparseSet* WorldStoragePtr : WorldRegistry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = WorldStoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& WorldStorage = *WorldStoragePtr;
                if (IsNonReplicatedStorage(ID))    continue;
                if (IsRuntimeOnlyComponent(ID))    continue;
                if (ID == TransformID)             continue;
                if (PrefabComponentIDs.find(ID) != PrefabComponentIDs.end()) continue;
                if (!WorldStorage.Contains(WorldE)) continue;

                // Keep instance-added components the prefab never shipped.
                if (NodeAdded != nullptr)
                {
                    if (CStruct* WS = FindComponentStructByTypeId(ID);
                        WS != nullptr && NodeAdded->find(WS->GetName()) != NodeAdded->end())
                    {
                        continue;
                    }
                }

                ToRemoveStorages.push_back(ID);
            }
            for (uint32 ID : ToRemoveStorages)
            {
                if (auto* Storage = WorldRegistry.FindStorage(ID))
                {
                    Storage->RemoveEntity(WorldE);
                }
            }

            // Prefab-authored handles never escape the prefab, so inherited handles resolve either way.
            ECS::Utils::RemapEntityReferences(WorldRegistry, WorldE, PrefabToInstance, /*bClearUnmapped*/ !bEntityHasOverrides);
        });

        // Replacing releases the SourcePrefab strong ref, and that transient zero can free the prefab mid-refresh.
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (!WorldRegistry.IsValid(WorldE)) continue;
            const bool bIsRoot = (WorldE == InstanceRoot);
            SPrefabInstanceComponent& Inst = WorldRegistry.GetOrEmplace<SPrefabInstanceComponent>(WorldE);
            Inst.SourcePrefab = this;
            Inst.StableID = StableID;
            Inst.bIsRoot = bIsRoot;
        }

        // Mirror the prefab's parent chain onto the instance; never reparent the placed root.
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (WorldE == InstanceRoot) continue;
            if (!WorldRegistry.IsValid(WorldE)) continue;

            const auto PrefabIt = PrefabByStableID.find(StableID);
            if (PrefabIt == PrefabByStableID.end()) continue;

            const FRelationshipComponent* PrefabRel = Registry.TryGet<FRelationshipComponent>(PrefabIt->second);
            const ECS::FEntity PrefabParent = PrefabRel ? PrefabRel->Parent : ECS::NullEntity;

            ECS::FEntity DesiredWorldParent = InstanceRoot;
            if (PrefabParent != ECS::NullEntity)
            {
                if (const SPrefabComponent* ParentTag = Registry.TryGet<SPrefabComponent>(PrefabParent))
                {
                    auto ParentIt = InstanceByStableID.find(ParentTag->StableID);
                    if (ParentIt != InstanceByStableID.end())
                    {
                        DesiredWorldParent = ParentIt->second;
                    }
                }
            }

            const FRelationshipComponent* CurrentRel = WorldRegistry.TryGet<FRelationshipComponent>(WorldE);
            const ECS::FEntity CurrentParent = CurrentRel ? CurrentRel->Parent : ECS::NullEntity;
            // ReparentEntity requires a transform on both sides, so a transform-less node would crash the refresh.
            if (CurrentParent != DesiredWorldParent
                && WorldRegistry.IsValid(DesiredWorldParent)
                && WorldRegistry.HasAll<STransformComponent>(WorldE)
                && WorldRegistry.HasAll<STransformComponent>(DesiredWorldParent))
            {
                ECS::Utils::ReparentEntity(WorldRegistry, WorldE, DesiredWorldParent);
            }
        }

        // Newly added entities + reparented ones need their world matrices recomputed.
        MarkSubtreeTransformsDirty(WorldRegistry, InstanceRoot);
    }

    void CPrefab::RefreshAllInstancesInWorld(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*World);

        // InitializeWorld culls the pending set pre-swap, but other paths reach this without that step.
        CullOrphanedInstances(WorldRegistry);

        TVector<ECS::FEntity> Roots;
        Roots.reserve(32);

        WorldRegistry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
        {
            if (Inst.bIsRoot && Inst.SourcePrefab != nullptr)
            {
                Roots.push_back(E);
            }
        });

        for (ECS::FEntity Root : Roots)
        {
            SPrefabInstanceComponent* Inst = WorldRegistry.TryGet<SPrefabInstanceComponent>(Root);
            if (Inst == nullptr || Inst->SourcePrefab == nullptr)
            {
                continue;
            }
            Inst->SourcePrefab->RefreshInstance(World, Root);
        }
    }

    void CPrefab::RefreshInstancesInLoadedWorlds()
    {
        if (GWorldManager == nullptr)
        {
            return;
        }

        // A world with no instance does no work, and the prefab editor's preview holds tags, not instances.
        for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
        {
            CWorld* World = Context ? Context->World.Get() : nullptr;
            if (World == nullptr)
            {
                continue;
            }

            ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*World);

            TVector<ECS::FEntity> Roots;
            WorldRegistry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
            {
                if (Inst.bIsRoot && Inst.SourcePrefab.Get() == this)
                {
                    Roots.push_back(E);
                }
            });

            for (ECS::FEntity Root : Roots)
            {
                RefreshInstance(World, Root);
            }
        }
    }

    void CPrefab::CullOrphanedInstances(ECS::FRegistry& Registry)
    {
        // SourcePrefab resolves to null or to a marked-destroy zombie, and either way the entity is garbage.
        TVector<ECS::FEntity> Orphans;
        Registry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
        {
            CPrefab* Src = Inst.SourcePrefab.Get();
            if (Src == nullptr || Src->HasAnyFlag(OF_MarkedDestroy))
            {
                Orphans.push_back(E);
            }
        });
        for (ECS::FEntity E : Orphans)
        {
            if (Registry.IsValid(E))
            {
                ECS::Utils::DestroyEntityHierarchy(Registry, E);
            }
        }
    }

    void CPrefab::DestroyAllInstancesInLoadedWorlds()
    {
        if (GWorldManager == nullptr)
        {
            return;
        }

        for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
        {
            CWorld* World = Context ? Context->World.Get() : nullptr;
            if (World == nullptr)
            {
                continue;
            }

            ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*World);

            // A detached subtree has no instance component, so it is not matched and survives.
            TVector<ECS::FEntity> Matching;
            WorldRegistry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
            {
                if (Inst.SourcePrefab.Get() == this)
                {
                    Matching.push_back(E);
                }
            });

            // Entries destroyed by an earlier iteration are skipped by the validity check, so no link dangles.
            for (ECS::FEntity E : Matching)
            {
                if (WorldRegistry.IsValid(E))
                {
                    ECS::Utils::DestroyEntityHierarchy(WorldRegistry, E);
                }
            }
        }
    }

    bool CPrefab::DetachInstance(CWorld* World, ECS::FEntity InstanceRoot)
    {
        if (World == nullptr)
        {
            return false;
        }

        ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*World);
        if (!WorldRegistry.IsValid(InstanceRoot))
        {
            return false;
        }

        const SPrefabInstanceComponent* RootInstance = WorldRegistry.TryGet<SPrefabInstanceComponent>(InstanceRoot);
        if (RootInstance == nullptr || !RootInstance->bIsRoot)
        {
            return false;
        }

        // Matched on the source, so another prefab's instance parented under this one stays linked to its own.
        CPrefab* const Source = RootInstance->SourcePrefab.Get();

        TVector<ECS::FEntity> ToStrip;
        ToStrip.reserve(16);
        ToStrip.push_back(InstanceRoot);
        ECS::Utils::ForEachDescendant(WorldRegistry, InstanceRoot, [&](ECS::FEntity Desc)
        {
            const SPrefabInstanceComponent* Inst = WorldRegistry.TryGet<SPrefabInstanceComponent>(Desc);
            if (Inst != nullptr && !Inst->bIsRoot && Inst->SourcePrefab.Get() == Source)
            {
                ToStrip.push_back(Desc);
            }
        });

        for (ECS::FEntity E : ToStrip)
        {
            WorldRegistry.Remove<SPrefabInstanceComponent>(E);
        }

        // The override ledger (root-only) is meaningless once detached.
        WorldRegistry.Remove<SPrefabOverrideComponent>(InstanceRoot);
        return true;
    }

    void CPrefab::CaptureFromWorld(CWorld* SourceWorld, ECS::FEntity RootEntity)
    {
        if (SourceWorld == nullptr)
        {
            return;
        }

        ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*SourceWorld);
        if (!WorldRegistry.IsValid(RootEntity))
        {
            return;
        }

        TVector<ECS::FEntity> EntitiesToCapture;
        EntitiesToCapture.reserve(16);
        EntitiesToCapture.push_back(RootEntity);
        ECS::Utils::ForEachDescendant(WorldRegistry, RootEntity, [&](ECS::FEntity E)
        {
            EntitiesToCapture.push_back(E);
        });

        // CopyRegistry remaps hierarchy and handle fields, and skips nested instance tracking.
        BumpDataGeneration();
        Registry = ECS::FRegistry{};
        THashMap<ECS::FEntity, ECS::FEntity> Map;
        CopyRegistry(WorldRegistry, Registry, Map, &EntitiesToCapture, &ShouldSkipInstanceComponent);

        // Reuses an existing instance tag when present, so RefreshInstance can match placed counterparts.
        for (ECS::FEntity SrcE : EntitiesToCapture)
        {
            auto It = Map.find(SrcE);
            if (It == Map.end())
            {
                continue;
            }

            FName StableID;
            if (const SPrefabInstanceComponent* Inst = WorldRegistry.TryGet<SPrefabInstanceComponent>(SrcE))
            {
                StableID = Inst->StableID;
            }
            if (StableID.IsNone())
            {
                StableID = GenerateStableID();
            }
            Registry.EmplaceOrReplace<SPrefabComponent>(It->second).StableID = StableID;
        }

        if (CPackage* Package = GetPackage())
        {
            Package->MarkDirty();
        }
    }

    namespace
    {
        THashMap<FName, ECS::FEntity> IndexByStableID(ECS::FRegistry& Registry)
        {
            THashMap<FName, ECS::FEntity> Out;
            Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity E, const SPrefabComponent& Comp)
            {
                if (!Comp.StableID.IsNone())
                {
                    Out.try_emplace(Comp.StableID, E);
                }
            });
            return Out;
        }

        FName StableIDOf(ECS::FRegistry& Registry, ECS::FEntity E)
        {
            const SPrefabComponent* Comp = Registry.IsValid(E) ? Registry.TryGet<SPrefabComponent>(E) : nullptr;
            return Comp != nullptr ? Comp->StableID : FName();
        }

        FName ParentStableIDOf(ECS::FRegistry& Registry, ECS::FEntity E)
        {
            const FRelationshipComponent* Rel = Registry.IsValid(E) ? Registry.TryGet<FRelationshipComponent>(E) : nullptr;
            if (Rel == nullptr || Rel->Parent == ECS::NullEntity)
            {
                return FName();
            }
            return StableIDOf(Registry, Rel->Parent);
        }

        CStruct* StructOfStorage(const ECS::FSparseSet& Storage)
        {
            return FindComponentStructByTypeId(Storage.GetTypeInfo().TypeID);
        }

        // The delta records these separately by StableID, so they are never diffed as component data.
        bool IsStructuralStorage(uint32 ID)
        {
            return ID == ECS::GetComponentTypeID<SPrefabComponent>()
                || ID == ECS::GetComponentTypeID<FRelationshipComponent>();
        }

        void RemapAllHandles(ECS::FRegistry& Registry, const THashMap<ECS::FEntity, ECS::FEntity>& Map)
        {
            Registry.ForEachEntity([&](ECS::FEntity E)
            {
                ECS::Utils::RemapEntityReferences(Registry, E, Map, /*bClearUnmapped*/ false);
            });
        }

        THashMap<ECS::FEntity, ECS::FEntity> InvertEntityMap(const THashMap<ECS::FEntity, ECS::FEntity>& Map)
        {
            THashMap<ECS::FEntity, ECS::FEntity> Out;
            for (const auto& [Key, Value] : Map)
            {
                Out[Value] = Key;
            }
            return Out;
        }

        // CopyRegistry nulls a handle whose target sits outside the copied set, so the targets join it.
        void AppendReferencedEntities(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Entities)
        {
            THashSet<ECS::FEntity> Present;
            Present.reserve(Entities.size());
            for (ECS::FEntity E : Entities)
            {
                Present.insert(E);
            }

            // Only the entities the caller supplied are walked, so an anchor cannot drag in a further one.
            TVector<ECS::FEntity> Referenced;
            const size_t OriginalCount = Entities.size();
            for (size_t i = 0; i < OriginalCount; ++i)
            {
                Referenced.clear();
                ECS::Utils::CollectEntityReferences(Registry, Entities[i], Referenced);
                for (ECS::FEntity Target : Referenced)
                {
                    if (Registry.IsValid(Target) && Present.insert(Target).second)
                    {
                        Entities.push_back(Target);
                    }
                }
            }
        }
    }

    bool CPrefab::IsDescendantOf(const CPrefab* Candidate) const
    {
        for (const CPrefab* Cur = this; Cur != nullptr; Cur = Cur->ParentPrefab.Get())
        {
            if (Cur == Candidate)
            {
                return true;
            }
        }
        return false;
    }

    TVector<CPrefab*> CPrefab::FindDirectVariants() const
    {
        TVector<CPrefab*> Out;
        for (TObjectIterator<CPrefab> It; It; ++It)
        {
            CPrefab* Candidate = *It;
            if (Candidate != this && Candidate->ParentPrefab.Get() == this)
            {
                Out.push_back(Candidate);
            }
        }
        return Out;
    }

    void CPrefab::ClearVariantDelta()
    {
        VariantDelta = ECS::FRegistry{};
        VariantOverriddenProperties.clear();
        VariantAddedComponents.clear();
        VariantRemovedComponents.clear();
        VariantStructuralNodes.clear();
        VariantRemovedEntities.clear();
    }

    void CPrefab::ResolveVariant()
    {
        TVector<const CPrefab*> VisitStack;
        ResolveVariantGuarded(VisitStack);
    }

    void CPrefab::ResolveVariantGuarded(TVector<const CPrefab*>& VisitStack)
    {
        if (!IsVariant())
        {
            return;
        }

        if (Algo::Contains(VisitStack, this))
        {
            LOG_ERROR("Prefab '{}' is in a variant cycle; leaving it unresolved.", GetName().c_str());
            bVariantResolveFailed = true;
            return;
        }

        CPrefab* Parent = ParentPrefab.Get();
        if (Parent == this || Parent->IsDescendantOf(this))
        {
            LOG_ERROR("Prefab '{}' would parent onto its own descendant; leaving it unresolved.", GetName().c_str());
            bVariantResolveFailed = true;
            return;
        }

        VisitStack.push_back(this);
        Parent->ResolveVariantGuarded(VisitStack);
        VisitStack.pop_back();

        // Resolving onto a parent that is itself empty would read as the variant having deleted everything.
        if (Parent->IsUnresolvedVariant())
        {
            LOG_ERROR("Prefab '{}' cannot resolve because its parent '{}' did not; leaving it unresolved.",
                GetName().c_str(), Parent->GetName().c_str());
            bVariantResolveFailed = true;
            return;
        }

        bVariantResolveFailed = false;

        BumpDataGeneration();
        ++VariantResolveCount;

        Registry = ECS::FRegistry{};
        THashMap<ECS::FEntity, ECS::FEntity> Map;
        CopyRegistry(Parent->Registry, Registry, Map);

        ApplyVariantDelta();
    }

    void CPrefab::ApplyVariantDelta()
    {

        THashMap<FName, ECS::FEntity> Resolved = IndexByStableID(Registry);

        // Surviving children are rescued to the root first, exactly as an instance refresh does.
        for (const FName& DeadID : VariantRemovedEntities)
        {
            auto It = Resolved.find(DeadID);
            if (It == Resolved.end() || !Registry.IsValid(It->second))
            {
                continue;
            }

            const ECS::FEntity Dead = It->second;
            TVector<ECS::FEntity> Survivors;
            ECS::Utils::ForEachDescendant(Registry, Dead, [&](ECS::FEntity Desc)
            {
                const FName DescID = StableIDOf(Registry, Desc);
                if (DescID.IsNone() || !Algo::Contains(VariantRemovedEntities, DescID))
                {
                    Survivors.push_back(Desc);
                }
            });
            for (ECS::FEntity S : Survivors)
            {
                if (Registry.IsValid(S) && Registry.HasAll<STransformComponent>(S))
                {
                    ECS::Utils::ReparentEntity(Registry, S, ECS::NullEntity);
                }
            }

            ECS::Utils::DestroyEntityHierarchy(Registry, Dead);
            Resolved.erase(It);
        }

        // An entity with no counterpart is one the variant adds, and the rest carry only its divergences.
        THashMap<FName, THashSet<FName>> RemovedByNode;
        for (const SPrefabComponentRef& Ref : VariantRemovedComponents)
        {
            RemovedByNode[Ref.EntityStableID].insert(Ref.ComponentType);
        }

        THashMap<FName, THashMap<FName, THashSet<FName>>> OverridesByNode;
        for (const SPrefabPropertyOverride& Override : VariantOverriddenProperties)
        {
            OverridesByNode[Override.EntityStableID][Override.ComponentType].insert(Override.PropertyPath);
        }

        // Added entities first, so a later reparent can address them.
        THashMap<ECS::FEntity, ECS::FEntity> DeltaToResolved;
        VariantDelta.View<SPrefabComponent>().ForEach([&](ECS::FEntity DeltaE, const SPrefabComponent& Comp)
        {
            if (Comp.StableID.IsNone())
            {
                return;
            }

            auto It = Resolved.find(Comp.StableID);
            if (It != Resolved.end())
            {
                DeltaToResolved[DeltaE] = It->second;
                return;
            }

            const ECS::FEntity NewE = Registry.Create();
            Registry.Emplace<SPrefabComponent>(NewE).StableID = Comp.StableID;
            Resolved[Comp.StableID] = NewE;
            DeltaToResolved[DeltaE] = NewE;
        });

        // A delta id means nothing in the resolved registry, which recycles the same small integers.
        RemapAllHandles(VariantDelta, DeltaToResolved);

        // A component absent from the resolved data is emplaced wholesale, otherwise only its leaves land.
        VariantDelta.View<SPrefabComponent>().ForEach([&](ECS::FEntity DeltaE, const SPrefabComponent& Comp)
        {
            auto DestIt = DeltaToResolved.find(DeltaE);
            if (DestIt == DeltaToResolved.end() || !Registry.IsValid(DestIt->second))
            {
                return;
            }

            const ECS::FEntity DestE = DestIt->second;
            const THashMap<FName, THashSet<FName>>* NodeOverrides = nullptr;
            if (auto OIt = OverridesByNode.find(Comp.StableID); OIt != OverridesByNode.end())
            {
                NodeOverrides = &OIt->second;
            }

            for (Lumina::ECS::FSparseSet* DeltaStoragePtr : VariantDelta.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = DeltaStoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& DeltaStorage = *DeltaStoragePtr;
                if (IsStructuralStorage(ID) || IsNonReplicatedStorage(ID)) continue;
                if (!DeltaStorage.Contains(DeltaE)) continue;

                CStruct* CompStruct = StructOfStorage(DeltaStorage);
                if (CompStruct == nullptr) continue;

                const FComponentOps* Ops = CompStruct->GetComponentOps();
                void* SrcPtr = DeltaStorage.GetRaw(DeltaE);

                void* DstPtr = nullptr;
                if (auto* DestStorage = Registry.FindStorage(ID); DestStorage != nullptr && DestStorage->Contains(DestE))
                {
                    DstPtr = DestStorage->GetRaw(DestE);
                }

                const THashSet<FName>* Leaves = nullptr;
                if (NodeOverrides != nullptr && CompStruct != nullptr)
                {
                    if (auto LIt = NodeOverrides->find(CompStruct->GetName()); LIt != NodeOverrides->end())
                    {
                        Leaves = &LIt->second;
                    }
                }

                // Otherwise write just the leaves the variant authors.
                if (DstPtr == nullptr || Leaves == nullptr || CompStruct == nullptr)
                {
                    Ops->EmplaceCopy(Registry, DestE, SrcPtr);
                }
                else
                {
                    PrefabOverride::ApplyOverriddenLeaves(CompStruct, DstPtr, SrcPtr, *Leaves);
                }
            }
        });

        // The delta is the persisted authority, and the next resolve builds a registry with different ids.
        RemapAllHandles(VariantDelta, InvertEntityMap(DeltaToResolved));

        // Components the variant deletes from an inherited node.
        for (auto& [NodeID, CompNames] : RemovedByNode)
        {
            auto It = Resolved.find(NodeID);
            if (It == Resolved.end() || !Registry.IsValid(It->second))
            {
                continue;
            }

            TVector<uint32> ToRemove;
            for (Lumina::ECS::FSparseSet* StoragePtr : Registry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& Storage = *StoragePtr;
                if (IsStructuralStorage(ID) || !Storage.Contains(It->second)) continue;

                CStruct* CompStruct = StructOfStorage(Storage);
                if (CompStruct != nullptr && CompNames.find(CompStruct->GetName()) != CompNames.end())
                {
                    ToRemove.push_back(ID);
                }
            }

            for (uint32 ID : ToRemove)
            {
                if (auto* Storage = Registry.FindStorage(ID))
                {
                    Storage->RemoveEntity(It->second);
                }
            }
        }

        // Parentage last, since every node the delta addresses now exists.
        for (const SPrefabVariantNode& Node : VariantStructuralNodes)
        {
            auto ChildIt = Resolved.find(Node.StableID);
            if (ChildIt == Resolved.end() || !Registry.IsValid(ChildIt->second))
            {
                continue;
            }

            // ReparentEntity reads the child's transform, and an added node may not have carried one.
            if (!Registry.HasAll<STransformComponent>(ChildIt->second))
            {
                Registry.Emplace<STransformComponent>(ChildIt->second);
            }

            if (Node.ParentStableID.IsNone())
            {
                ECS::Utils::ReparentEntity(Registry, ChildIt->second, ECS::NullEntity);
                continue;
            }

            auto ParentIt = Resolved.find(Node.ParentStableID);
            if (ParentIt != Resolved.end() && Registry.IsValid(ParentIt->second))
            {
                ECS::Utils::ReparentEntity(Registry, ChildIt->second, ParentIt->second);
            }
        }
    }

    void CPrefab::CaptureVariantDelta()
    {
        if (!IsVariant())
        {
            return;
        }

        CPrefab* Parent = ParentPrefab.Get();
        Parent->ResolveVariant();

        // Diffing an unresolved side would persist a delta that deletes or re-adds the whole prefab.
        if (bVariantResolveFailed || Parent->IsUnresolvedVariant())
        {
            LOG_ERROR("Prefab '{}' is unresolved against its parent; keeping its stored delta rather than "
                      "overwriting it with a diff of empty data.", GetName().c_str());
            return;
        }

        ClearVariantDelta();
        BumpDataGeneration();

        THashMap<FName, ECS::FEntity> ParentByID = IndexByStableID(Parent->Registry);
        THashMap<FName, ECS::FEntity> MineByID   = IndexByStableID(Registry);

        // Entities the parent still has and this variant dropped.
        for (auto& [StableID, ParentE] : ParentByID)
        {
            if (MineByID.find(StableID) == MineByID.end())
            {
                VariantRemovedEntities.push_back(StableID);
            }
        }

        // Collected first so the delta registry is built in one copy.
        TVector<ECS::FEntity> DivergedEntities;
        THashMap<FName, THashSet<FName>> KeepComponentsByNode;

        for (auto& [StableID, MineE] : MineByID)
        {
            auto ParentIt = ParentByID.find(StableID);
            const bool bAdded = ParentIt == ParentByID.end();

            if (bAdded)
            {
                DivergedEntities.push_back(MineE);
                VariantStructuralNodes.push_back(SPrefabVariantNode{ StableID, ParentStableIDOf(Registry, MineE), true });
                continue;
            }

            const ECS::FEntity ParentE = ParentIt->second;

            if (ParentStableIDOf(Registry, MineE) != ParentStableIDOf(Parent->Registry, ParentE))
            {
                VariantStructuralNodes.push_back(SPrefabVariantNode{ StableID, ParentStableIDOf(Registry, MineE), false });
            }

            bool bNodeDiverged = false;

            // Components this variant carries, added outright or diverged on some leaf.
            for (Lumina::ECS::FSparseSet* MyStoragePtr : Registry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = MyStoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& MyStorage = *MyStoragePtr;
                if (IsStructuralStorage(ID) || IsNonReplicatedStorage(ID)) continue;
                if (!MyStorage.Contains(MineE)) continue;

                CStruct* CompStruct = StructOfStorage(MyStorage);
                if (CompStruct == nullptr) continue;

                auto* ParentStorage = Parent->Registry.FindStorage(ID);
                const bool bParentHas = ParentStorage != nullptr && ParentStorage->Contains(ParentE);

                if (!bParentHas)
                {
                    VariantAddedComponents.push_back(SPrefabComponentRef{ StableID, CompStruct->GetName() });
                    KeepComponentsByNode[StableID].insert(CompStruct->GetName());
                    bNodeDiverged = true;
                    continue;
                }

                TVector<FName> Leaves;
                PrefabOverride::CollectOverriddenLeaves(CompStruct, MyStorage.GetRaw(MineE),
                    ParentStorage->GetRaw(ParentE), Leaves);

                for (const FName& Path : Leaves)
                {
                    VariantOverriddenProperties.push_back(SPrefabPropertyOverride{ StableID, CompStruct->GetName(), Path });
                }

                if (!Leaves.empty())
                {
                    KeepComponentsByNode[StableID].insert(CompStruct->GetName());
                    bNodeDiverged = true;
                }
            }

            // Components the parent ships and this variant deleted.
            for (Lumina::ECS::FSparseSet* ParentStoragePtr : Parent->Registry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = ParentStoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& ParentStorage = *ParentStoragePtr;
                if (IsStructuralStorage(ID) || IsNonReplicatedStorage(ID)) continue;
                if (!ParentStorage.Contains(ParentE)) continue;

                auto* MyStorage = Registry.FindStorage(ID);
                if (MyStorage != nullptr && MyStorage->Contains(MineE)) continue;

                if (CStruct* CompStruct = StructOfStorage(ParentStorage))
                {
                    VariantRemovedComponents.push_back(SPrefabComponentRef{ StableID, CompStruct->GetName() });
                }
            }

            if (bNodeDiverged)
            {
                DivergedEntities.push_back(MineE);
            }
        }

        if (DivergedEntities.empty())
        {
            return;
        }

        // The strip pass below leaves an anchor bare, so it costs an id and nothing else.
        AppendReferencedEntities(Registry, DivergedEntities);

        THashMap<ECS::FEntity, ECS::FEntity> Map;
        CopyRegistry(Registry, VariantDelta, Map, &DivergedEntities);

        // The delta stores authored values only, and carrying the rest would freeze them against parent edits.
        VariantDelta.View<SPrefabComponent>().ForEach([&](ECS::FEntity DeltaE, const SPrefabComponent& Comp)
        {
            auto KeepIt = KeepComponentsByNode.find(Comp.StableID);
            const bool bAddedNode = Algo::AnyOf(VariantStructuralNodes,
                [&](const SPrefabVariantNode& N) { return N.bAdded && N.StableID == Comp.StableID; });

            if (bAddedNode)
            {
                return; // an added node authors everything it carries
            }

            TVector<uint32> ToStrip;
            for (Lumina::ECS::FSparseSet* StoragePtr : VariantDelta.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& Storage = *StoragePtr;
                if (IsStructuralStorage(ID) || !Storage.Contains(DeltaE)) continue;

                CStruct* CompStruct = StructOfStorage(Storage);
                const bool bKeep = CompStruct != nullptr
                    && KeepIt != KeepComponentsByNode.end()
                    && KeepIt->second.find(CompStruct->GetName()) != KeepIt->second.end();

                if (!bKeep)
                {
                    ToStrip.push_back(ID);
                }
            }

            for (uint32 ID : ToStrip)
            {
                if (auto* Storage = VariantDelta.FindStorage(ID))
                {
                    Storage->RemoveEntity(DeltaE);
                }
            }
        });
    }

    void CPrefab::PropagateToVariants()
    {
        // Each level is gathered before it resolves, since resolving loads nothing but the list must be stable.
        TVector<CPrefab*> Frontier = FindDirectVariants();
        THashSet<CPrefab*> Seen;

        while (!Frontier.empty())
        {
            TVector<CPrefab*> Next;
            for (CPrefab* Variant : Frontier)
            {
                if (!Seen.insert(Variant).second)
                {
                    continue;
                }

                Variant->ResolveVariant();
                Variant->RefreshInstancesInLoadedWorlds();

                for (CPrefab* Child : Variant->FindDirectVariants())
                {
                    Next.push_back(Child);
                }
            }
            Frontier = Move(Next);
        }
    }

    void CPrefab::RebuildStableIDLookup()
    {
        StableIDLookup.clear();
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity E, const SPrefabComponent& Comp)
        {
            if (!Comp.StableID.IsNone())
            {
                // A duplicate id is a bug elsewhere, and picking the same entity keeps the baseline stable.
                StableIDLookup.try_emplace(Comp.StableID, E);
            }
        });
    }

    ECS::FEntity CPrefab::FindEntityByStableID(const FName& StableID)
    {
        if (StableID.IsNone())
        {
            return ECS::NullEntity;
        }

        // A global counter, so an unrelated edit costs one rebuild, cheap next to rescanning per property.
        const uint32 Generation = GetDataGeneration();
        if (!bStableIDLookupBuilt || StableIDLookupGeneration != Generation)
        {
            RebuildStableIDLookup();
            StableIDLookupGeneration = Generation;
            bStableIDLookupBuilt     = true;
        }

        auto It = StableIDLookup.find(StableID);
        if (It == StableIDLookup.end())
        {
            return ECS::NullEntity;
        }

        // A missed generation bump would hand back a recycled entity, and a wrong component is worse than none.
        const ECS::FEntity Cached = It->second;
        if (!Registry.IsValid(Cached))
        {
            return ECS::NullEntity;
        }

        const SPrefabComponent* Comp = Registry.TryGet<SPrefabComponent>(Cached);
        return (Comp != nullptr && Comp->StableID == StableID) ? Cached : ECS::NullEntity;
    }

    void* CPrefab::ResolvePrefabComponentPtr(const FName& StableID, CStruct* Struct)
    {
        if (Struct == nullptr || StableID.IsNone())
        {
            return nullptr;
        }

        const ECS::FEntity PrefabE = FindEntityByStableID(StableID);
        if (PrefabE == ECS::NullEntity)
        {
            return nullptr;
        }

        return FindReflectedComponentPtr(Registry, PrefabE, Struct);
    }

    ECS::FEntity CPrefab::FindInstanceRoot(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        ECS::FEntity Cur = Entity;
        while (Registry.IsValid(Cur))
        {
            const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Cur);
            if (Inst == nullptr)
            {
                return ECS::NullEntity;
            }
            if (Inst->bIsRoot)
            {
                return Cur;
            }
            const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Cur);
            Cur = Rel ? Rel->Parent : ECS::NullEntity;
        }
        return ECS::NullEntity;
    }

    void CPrefab::RecaptureComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || !Registry.IsValid(Entity))
        {
            return;
        }

        const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Entity);
        if (Inst == nullptr || Inst->SourcePrefab == nullptr)
        {
            return;
        }

        const ECS::FEntity Root = FindInstanceRoot(Registry, Entity);
        if (Root == ECS::NullEntity)
        {
            return;
        }

        const FName NodeID = Inst->StableID;
        const FName CompName = ComponentType->GetName();

        // Live instance component value.
        void* InstPtr = FindReflectedComponentPtr(Registry, Entity, ComponentType);

        // Prefab baseline value (null when the component is instance-added => no per-leaf tracking).
        void* PrefPtr = Inst->SourcePrefab->ResolvePrefabComponentPtr(NodeID, ComponentType);

        TVector<FName> NewPaths;
        if (InstPtr != nullptr && PrefPtr != nullptr)
        {
            PrefabOverride::CollectOverriddenLeaves(ComponentType, InstPtr, PrefPtr, NewPaths);
        }

        SPrefabOverrideComponent& Ledger = Registry.GetOrEmplace<SPrefabOverrideComponent>(Root);

        // Replace this (node, component) pair's records with the freshly computed set.
        auto& Recs = Ledger.PropertyOverrides;
        Recs.erase(Algo::RemoveIf(Recs, [&](const SPrefabPropertyOverride& O)
        {
            return O.EntityStableID == NodeID && O.ComponentType == CompName;
        }), Recs.end());

        for (const FName& Path : NewPaths)
        {
            SPrefabPropertyOverride Rec;
            Rec.EntityStableID = NodeID;
            Rec.ComponentType  = CompName;
            Rec.PropertyPath   = Path;
            Recs.push_back(Rec);
        }
    }

    void CPrefab::NoteComponentAdded(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || !Registry.IsValid(Entity))
        {
            return;
        }

        const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Entity);
        if (Inst == nullptr)
        {
            return;
        }
        const ECS::FEntity Root = FindInstanceRoot(Registry, Entity);
        if (Root == ECS::NullEntity)
        {
            return;
        }

        const FName NodeID = Inst->StableID;
        const FName CompName = ComponentType->GetName();
        const bool bPrefabHas = Inst->SourcePrefab != nullptr
            && Inst->SourcePrefab->ResolvePrefabComponentPtr(NodeID, ComponentType) != nullptr;

        SPrefabOverrideComponent& Ledger = Registry.GetOrEmplace<SPrefabOverrideComponent>(Root);

        auto MatchesPair = [&](const SPrefabComponentRef& C)
        {
            return C.EntityStableID == NodeID && C.ComponentType == CompName;
        };

        // A genuinely new component is recorded as instance-added, so refresh never prunes it.
        auto& Removed = Ledger.RemovedComponents;
        Removed.erase(Algo::RemoveIf(Removed, MatchesPair), Removed.end());

        if (!bPrefabHas)
        {
            auto& Added = Ledger.AddedComponents;
            if (!Algo::AnyOf(Added, MatchesPair))
            {
                SPrefabComponentRef Rec;
                Rec.EntityStableID = NodeID;
                Rec.ComponentType  = CompName;
                Added.push_back(Rec);
            }
        }
    }

    void CPrefab::NoteComponentRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || !Registry.IsValid(Entity))
        {
            return;
        }

        const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Entity);
        if (Inst == nullptr)
        {
            return;
        }
        const ECS::FEntity Root = FindInstanceRoot(Registry, Entity);
        if (Root == ECS::NullEntity)
        {
            return;
        }

        const FName NodeID = Inst->StableID;
        const FName CompName = ComponentType->GetName();
        const bool bPrefabHas = Inst->SourcePrefab != nullptr
            && Inst->SourcePrefab->ResolvePrefabComponentPtr(NodeID, ComponentType) != nullptr;

        SPrefabOverrideComponent& Ledger = Registry.GetOrEmplace<SPrefabOverrideComponent>(Root);

        auto MatchesPair = [&](const auto& C)
        {
            return C.EntityStableID == NodeID && C.ComponentType == CompName;
        };

        // Any property overrides for the gone component are meaningless now.
        auto& Props = Ledger.PropertyOverrides;
        Props.erase(Algo::RemoveIf(Props, MatchesPair), Props.end());

        auto& Added = Ledger.AddedComponents;
        Added.erase(Algo::RemoveIf(Added, MatchesPair), Added.end());

        auto& Removed = Ledger.RemovedComponents;
        Removed.erase(Algo::RemoveIf(Removed, MatchesPair), Removed.end());

        // An inherited component the user deleted must be recorded so refresh won't re-add it.
        if (bPrefabHas)
        {
            SPrefabComponentRef Rec;
            Rec.EntityStableID = NodeID;
            Rec.ComponentType  = CompName;
            Removed.push_back(Rec);
        }
    }
}
