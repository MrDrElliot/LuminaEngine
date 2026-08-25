#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
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
#include "World/Entity/EntityUtils.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        /** Component types skipped by the cross-registry copy pass. */
        bool IsNonReplicatedStorage(entt::id_type ID)
        {
            // Relationships are remapped manually after the copy pass.
            if (ID == entt::type_hash<FRelationshipComponent>::value()) return true;
            // Editor-only state must not leak between worlds and prefabs.
            if (ID == entt::type_hash<FSelectedInEditorComponent>::value()) return true;
            if (ID == entt::type_hash<FHideInSceneOutliner>::value()) return true;
            if (ID == entt::type_hash<FEditorComponent>::value()) return true;
            return false;
        }

        // Runtime-only / spawn-tagging components a refresh diff must never remove; they'd re-spawn next frame.
        bool IsRuntimeOnlyComponent(entt::id_type ID)
        {
            if (ID == entt::type_hash<SPrefabInstanceComponent>::value()) return true;
            // The ledger lives on the instance root and is absent from the prefab, so it must survive every refresh.
            if (ID == entt::type_hash<SPrefabOverrideComponent>::value()) return true;
            if (ID == entt::type_hash<SRigidBodyComponent>::value())      return true;
            if (ID == entt::type_hash<FNeedsTransformUpdate>::value())    return true;
            if (ID == entt::type_hash<FNeedsPhysicsBodyUpdate>::value())  return true;
            return false;
        }

        FName GenerateStableID()
        {
            return FName(FGuid::New().ToShortString());
        }

        // Game thread only, since every writer runs from world init, the prefab editor or asset capture.
        uint32 GDataGeneration = 0;

        // Nested instance tracking must not leak into the new prefab, which gets fresh tags instead.
        bool ShouldSkipInstanceComponent(entt::id_type ID)
        {
            return ID == entt::type_hash<SPrefabInstanceComponent>::value()
                || ID == entt::type_hash<SPrefabOverrideComponent>::value();
        }
        
        void* FindReflectedComponentPtr(entt::registry& Registry, entt::entity Entity, CStruct* Struct)
        {
            if (Struct == nullptr || !Registry.valid(Entity))
            {
                return nullptr;
            }
            entt::meta_type Meta = entt::resolve(ECS::Utils::GetTypeID(Struct));
            if (!Meta)
            {
                return nullptr;
            }
            if (auto* Storage = Registry.storage(Meta.info().hash()))
            {
                if (Storage->contains(Entity))
                {
                    return Storage->value(Entity);
                }
            }
            return nullptr;
        }

        // Otherwise freshly spawned entities render at a stale position for one frame.
        void MarkSubtreeTransformsDirty(entt::registry& Registry, entt::entity Root)
        {
            if (!Registry.valid(Root))
            {
                return;
            }
            Registry.emplace_or_replace<FNeedsTransformUpdate>(Root);
            ECS::Utils::ForEachDescendant(Registry, Root, [&](entt::entity Desc)
            {
                Registry.emplace_or_replace<FNeedsTransformUpdate>(Desc);
            });
        }
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
        bool bVariantPayload = IsVariant();
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::PREFAB_VARIANTS)
        {
            Ar << bVariantPayload;
        }
        else
        {
            bVariantPayload = false;
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
    }

    void CPrefab::CopyRegistry(entt::registry& Source, entt::registry& Dest, THashMap<entt::entity, entt::entity>& OutMap,
        const TVector<entt::entity>* SourceEntities, bool(*ExtraSkipStorage)(entt::id_type))
    {
        using namespace entt::literals;

        if (SourceEntities != nullptr)
        {
            for (entt::entity SrcE : *SourceEntities)
            {
                if (Source.valid(SrcE))
                {
                    OutMap[SrcE] = Dest.create();
                }
            }
        }
        else
        {
            Source.view<entt::entity>().each([&](entt::entity SrcE)
            {
                OutMap[SrcE] = Dest.create();
            });
        }

        for (auto&& [ID, SrcSet] : Source.storage())
        {
            // Rigid bodies carry a runtime BodyID that must not be copied; handled below.
            if (IsNonReplicatedStorage(ID)
                || ID == entt::type_hash<SRigidBodyComponent>::value()
                || (ExtraSkipStorage != nullptr && ExtraSkipStorage(ID)))
            {
                continue;
            }

            entt::meta_type MetaType = entt::resolve(SrcSet.info());
            if (!MetaType)
            {
                continue;
            }

            for (entt::entity SrcE : SrcSet)
            {
                auto It = OutMap.find(SrcE);
                if (It == OutMap.end())
                {
                    continue;
                }

                entt::entity DestE = It->second;
                void* SrcCompPtr = SrcSet.value(SrcE);

                entt::meta_any SrcAny = MetaType.from_void(SrcCompPtr);
                ECS::Utils::InvokeMetaFunc(MetaType, "emplace"_hs,
                    entt::forward_as_meta(Dest), DestE, entt::forward_as_meta(SrcAny));
            }
        }

        // Copying the live body id makes the physics scene skip creation as already existing.
        for (auto& [SrcE, DestE] : OutMap)
        {
            if (const SRigidBodyComponent* SrcBody = Source.try_get<SRigidBodyComponent>(SrcE))
            {
                SRigidBodyComponent NewBody = *SrcBody;
                NewBody.BodyID = 0xFFFFFFFFu;
                Dest.emplace_or_replace<SRigidBodyComponent>(DestE, NewBody);
            }
        }

        auto Remap = [&](entt::entity& E)
        {
            if (E != entt::null)
            {
                auto It = OutMap.find(E);
                E = (It != OutMap.end()) ? It->second : entt::null;
            }
        };

        Source.view<FRelationshipComponent>().each([&](entt::entity SrcE, const FRelationshipComponent& SrcRel)
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
            Dest.emplace_or_replace<FRelationshipComponent>(It->second, DestRel);
        });

        // References escaping the copied set are cleared, so a stale id cannot alias an unrelated entity.
        for (auto& [SrcE, DestE] : OutMap)
        {
            ECS::Utils::RemapEntityReferences(Dest, DestE, OutMap, /*bClearUnmapped*/ true);
        }
        
        for (auto& [SrcE, DestE] : OutMap)
        {
            if (STransformComponent* DestTransform = Dest.try_get<STransformComponent>(DestE))
            {
                DestTransform->Bind(Dest, DestE);
                DestTransform->ResetDirtyState();
            }
        }
    }

    entt::entity CPrefab::Instantiate(CWorld* TargetWorld, const FTransform& OffsetTransform, entt::entity Parent)
    {
        if (TargetWorld == nullptr)
        {
            return entt::null;
        }

        if (bVariantResolveFailed)
        {
            LOG_WARN("Prefab '{}' did not resolve against its parent; refusing to instantiate it.", GetName().c_str());
            return entt::null;
        }

        LUMINA_PROFILE_SCOPE();

        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*TargetWorld);
        
        TVector<entt::entity> PrefabRoots;
        PrefabRoots.reserve(2);
        Registry.view<entt::entity>().each([&](entt::entity E)
        {
            const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(E);
            const bool bHasParent = Rel && Rel->Parent != entt::null;
            if (!bHasParent)
            {
                PrefabRoots.push_back(E);
            }
        });

        if (PrefabRoots.empty())
        {
            LOG_WARN("Prefab '{}' has no entities; nothing to instantiate.", GetName().c_str());
            return entt::null;
        }

        const entt::entity PrefabRoot = PrefabRoots[0];
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

        THashMap<entt::entity, entt::entity> Map;
        entt::entity WorldRoot = entt::null;

        {
            FBodyBatchScope BodyBatch(TargetWorld->GetPhysicsScene());

            CopyRegistry(Registry, WorldRegistry, Map);

            WorldRoot = Map[PrefabRoot];

            for (auto& [SrcE, DestE] : Map)
            {
                FName StableID;
                if (const SPrefabComponent* PrefabComp = WorldRegistry.try_get<SPrefabComponent>(DestE))
                {
                    StableID = PrefabComp->StableID;
                }
                else
                {
                    StableID = GenerateStableID();
                }

                WorldRegistry.remove<SPrefabComponent>(DestE);

                SPrefabInstanceComponent& Instance = WorldRegistry.emplace_or_replace<SPrefabInstanceComponent>(DestE);
                Instance.SourcePrefab = this;
                Instance.StableID = StableID;
                Instance.bIsRoot = (DestE == WorldRoot);
            }

            // Rescue any extra parentless entities so the spawn has a single hierarchical root.
            for (size_t i = 1; i < PrefabRoots.size(); ++i)
            {
                const entt::entity Extra = Map[PrefabRoots[i]];
                if (Extra != entt::null && WorldRegistry.valid(Extra))
                {
                    ECS::Utils::ReparentEntity(WorldRegistry, Extra, WorldRoot);
                }
            }

            if (STransformComponent* RootTransform = WorldRegistry.try_get<STransformComponent>(WorldRoot))
            {
                RootTransform->SetLocalTransform(OffsetTransform);
            }
            else
            {
                WorldRegistry.emplace<STransformComponent>(WorldRoot, OffsetTransform);
            }

            if (Parent != entt::null && WorldRegistry.valid(Parent))
            {
                ECS::Utils::ReparentEntity(WorldRegistry, WorldRoot, Parent);
            }
            
            MarkSubtreeTransformsDirty(WorldRegistry, WorldRoot);
        }

        return WorldRoot;
    }

    void CPrefab::RefreshInstance(CWorld* World, entt::entity InstanceRoot)
    {
        using namespace entt::literals;

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

        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);
        if (!WorldRegistry.valid(InstanceRoot))
        {
            return;
        }

        SPrefabInstanceComponent* RootInstance = WorldRegistry.try_get<SPrefabInstanceComponent>(InstanceRoot);
        if (RootInstance == nullptr || RootInstance->SourcePrefab != this)
        {
            return;
        }

        // Everything past here rewrites this instance's components, invalidating any cached pointer into them.
        BumpDataGeneration();

        // Index instance entities by StableID.
        THashMap<FName, entt::entity> InstanceByStableID;
        InstanceByStableID[RootInstance->StableID] = InstanceRoot;
        ECS::Utils::ForEachDescendant(WorldRegistry, InstanceRoot, [&](entt::entity Descendant)
        {
            if (const SPrefabInstanceComponent* Inst = WorldRegistry.try_get<SPrefabInstanceComponent>(Descendant))
            {
                if (Inst->SourcePrefab == this && !Inst->StableID.IsNone())
                {
                    InstanceByStableID[Inst->StableID] = Descendant;
                }
            }
        });

        // Index prefab entities by StableID.
        THashMap<FName, entt::entity> PrefabByStableID;
        Registry.view<SPrefabComponent>().each([&](entt::entity PrefabE, const SPrefabComponent& PrefabComp)
        {
            if (!PrefabComp.StableID.IsNone())
            {
                PrefabByStableID[PrefabComp.StableID] = PrefabE;
            }
        });

        // Destroy instance entities whose prefab counterpart is gone (never the user-placed root).
        TVector<entt::entity> ToDestroy;
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
        THashSet<entt::entity> DeadSet;
        DeadSet.reserve(ToDestroy.size());
        for (entt::entity E : ToDestroy)
        {
            DeadSet.insert(E);
        }
        for (entt::entity Dead : ToDestroy)
        {
            if (!WorldRegistry.valid(Dead))
            {
                continue;
            }
            TVector<entt::entity> Survivors;
            ECS::Utils::ForEachDescendant(WorldRegistry, Dead, [&](entt::entity Desc)
            {
                if (DeadSet.find(Desc) == DeadSet.end())
                {
                    Survivors.push_back(Desc);
                }
            });
            for (entt::entity S : Survivors)
            {
                if (WorldRegistry.valid(S) && WorldRegistry.all_of<STransformComponent>(S))
                {
                    ECS::Utils::ReparentEntity(WorldRegistry, S, InstanceRoot);
                }
            }
        }

        for (entt::entity E : ToDestroy)
        {
            const auto It = Algo::FindIf(InstanceByStableID.begin(), InstanceByStableID.end(),
                [&](const auto& Pair) { return Pair.second == E; });
            if (It != InstanceByStableID.end())
            {
                InstanceByStableID.erase(It);
            }
            if (WorldRegistry.valid(E))
            {
                ECS::Utils::DestroyEntityHierarchy(WorldRegistry, E);
            }
        }

        // Spawn instance entities for new prefab entries.
        for (auto& [StableID, PrefabE] : PrefabByStableID)
        {
            if (InstanceByStableID.find(StableID) != InstanceByStableID.end())
            {
                continue;
            }
            const entt::entity NewE = WorldRegistry.create();
            InstanceByStableID[StableID] = NewE;

            SPrefabInstanceComponent& Inst = WorldRegistry.emplace<SPrefabInstanceComponent>(NewE);
            Inst.SourcePrefab = this;
            Inst.StableID     = StableID;
            Inst.bIsRoot      = false;
        }

        // Prefab-entity -> instance-entity remap table (for entity-handle fields).
        THashMap<entt::entity, entt::entity> PrefabToInstance;
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
        if (const SPrefabOverrideComponent* Ledger = WorldRegistry.try_get<SPrefabOverrideComponent>(InstanceRoot))
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
        const entt::id_type TransformID = entt::type_hash<STransformComponent>::value();
        const entt::id_type ScriptComponentID = entt::type_hash<SEntityScriptComponent>::value();

        // entt's replace raises on_update rather than on_destroy, so a script's OnDetach is skipped.
        auto EmplaceFromPrefab = [&](entt::id_type ID, const entt::meta_type& MetaType, entt::entity WorldE, void* SrcCompPtr)
        {
            using namespace entt::literals;
            if (ID == ScriptComponentID)
            {
                EntityScripts::DetachAll(WorldRegistry, WorldE);
            }
            entt::meta_any SrcAny = MetaType.from_void(SrcCompPtr);
            ECS::Utils::InvokeMetaFunc(MetaType, "emplace"_hs,
                entt::forward_as_meta(WorldRegistry), WorldE, entt::forward_as_meta(SrcAny));
        };

        // Copy/replace components prefab -> instance honoring overrides, then prune ones the prefab dropped.
        Registry.view<SPrefabComponent>().each([&](entt::entity PrefabE, const SPrefabComponent& PrefabComp)
        {
            auto It = InstanceByStableID.find(PrefabComp.StableID);
            if (It == InstanceByStableID.end())
            {
                return;
            }

            const entt::entity WorldE = It->second;
            if (!WorldRegistry.valid(WorldE))
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
            THashSet<entt::id_type> PrefabComponentIDs;
            PrefabComponentIDs.reserve(8);

            for (auto&& [ID, PrefabStorage] : Registry.storage())
            {
                if (IsNonReplicatedStorage(ID)) continue;
                if (ID == entt::type_hash<SPrefabComponent>::value()) continue;
                if (!PrefabStorage.contains(PrefabE)) continue;

                entt::meta_type MetaType = entt::resolve(PrefabStorage.info());
                if (!MetaType) continue;

                void* SrcCompPtr = PrefabStorage.value(PrefabE);

                // A spawned entity left with no transform would crash the hierarchy-mirror reparent.
                if (ID == TransformID)
                {
                    auto* WorldXform = WorldRegistry.storage(ID);
                    if (WorldXform == nullptr || !WorldXform->contains(WorldE))
                    {
                        entt::meta_any SrcAny = MetaType.from_void(SrcCompPtr);
                        ECS::Utils::InvokeMetaFunc(MetaType, "emplace"_hs,
                            entt::forward_as_meta(WorldRegistry), WorldE, entt::forward_as_meta(SrcAny));
                    }
                    continue;
                }

                // A node with no ledger refreshes exactly as before.
                if (!bNodeHasLedger)
                {
                    PrefabComponentIDs.insert(ID);
                    EmplaceFromPrefab(ID, MetaType, WorldE, SrcCompPtr);
                    continue;
                }

                CStruct* CompStruct = nullptr;
                if (entt::meta_any S = ECS::Utils::InvokeMetaFunc(MetaType, "static_struct"_hs))
                {
                    CompStruct = S.cast<CStruct*>();
                }
                const FName CompName = CompStruct ? CompStruct->GetName() : FName();

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
                if (auto* WorldStorage = WorldRegistry.storage(ID))
                {
                    if (WorldStorage->contains(WorldE))
                    {
                        DstCompPtr = WorldStorage->value(WorldE);
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
                    EmplaceFromPrefab(ID, MetaType, WorldE, SrcCompPtr);
                }
            }

            // Skips non-replicated, transform and instance-added components.
            TVector<entt::id_type> ToRemoveStorages;
            for (auto&& [ID, WorldStorage] : WorldRegistry.storage())
            {
                if (IsNonReplicatedStorage(ID))    continue;
                if (IsRuntimeOnlyComponent(ID))    continue;
                if (ID == TransformID)             continue;
                if (PrefabComponentIDs.find(ID) != PrefabComponentIDs.end()) continue;
                if (!WorldStorage.contains(WorldE)) continue;

                // Keep instance-added components the prefab never shipped.
                if (NodeAdded != nullptr)
                {
                    if (entt::meta_type WorldMeta = entt::resolve(WorldStorage.info()))
                    {
                        if (entt::meta_any S = ECS::Utils::InvokeMetaFunc(WorldMeta, "static_struct"_hs))
                        {
                            if (CStruct* WS = S.cast<CStruct*>(); WS && NodeAdded->find(WS->GetName()) != NodeAdded->end())
                            {
                                continue;
                            }
                        }
                    }
                }

                ToRemoveStorages.push_back(ID);
            }
            for (entt::id_type ID : ToRemoveStorages)
            {
                if (auto* Storage = WorldRegistry.storage(ID))
                {
                    Storage->remove(WorldE);
                }
            }

            // Prefab-authored handles never escape the prefab, so inherited handles resolve either way.
            ECS::Utils::RemapEntityReferences(WorldRegistry, WorldE, PrefabToInstance, /*bClearUnmapped*/ !bEntityHasOverrides);
        });

        // Replacing releases the SourcePrefab strong ref, and that transient zero can free the prefab mid-refresh.
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (!WorldRegistry.valid(WorldE)) continue;
            const bool bIsRoot = (WorldE == InstanceRoot);
            SPrefabInstanceComponent& Inst = WorldRegistry.get_or_emplace<SPrefabInstanceComponent>(WorldE);
            Inst.SourcePrefab = this;
            Inst.StableID = StableID;
            Inst.bIsRoot = bIsRoot;
        }

        // Mirror the prefab's parent chain onto the instance; never reparent the placed root.
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (WorldE == InstanceRoot) continue;
            if (!WorldRegistry.valid(WorldE)) continue;

            const auto PrefabIt = PrefabByStableID.find(StableID);
            if (PrefabIt == PrefabByStableID.end()) continue;

            const FRelationshipComponent* PrefabRel = Registry.try_get<FRelationshipComponent>(PrefabIt->second);
            const entt::entity PrefabParent = PrefabRel ? PrefabRel->Parent : entt::null;

            entt::entity DesiredWorldParent = InstanceRoot;
            if (PrefabParent != entt::null)
            {
                if (const SPrefabComponent* ParentTag = Registry.try_get<SPrefabComponent>(PrefabParent))
                {
                    auto ParentIt = InstanceByStableID.find(ParentTag->StableID);
                    if (ParentIt != InstanceByStableID.end())
                    {
                        DesiredWorldParent = ParentIt->second;
                    }
                }
            }

            const FRelationshipComponent* CurrentRel = WorldRegistry.try_get<FRelationshipComponent>(WorldE);
            const entt::entity CurrentParent = CurrentRel ? CurrentRel->Parent : entt::null;
            // ReparentEntity requires a transform on both sides, so a transform-less node would crash the refresh.
            if (CurrentParent != DesiredWorldParent
                && WorldRegistry.valid(DesiredWorldParent)
                && WorldRegistry.all_of<STransformComponent>(WorldE)
                && WorldRegistry.all_of<STransformComponent>(DesiredWorldParent))
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

        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);

        // InitializeWorld culls the pending set pre-swap, but other paths reach this without that step.
        CullOrphanedInstances(WorldRegistry);

        TVector<entt::entity> Roots;
        Roots.reserve(32);

        WorldRegistry.view<SPrefabInstanceComponent>().each([&](entt::entity E, const SPrefabInstanceComponent& Inst)
        {
            if (Inst.bIsRoot && Inst.SourcePrefab != nullptr)
            {
                Roots.push_back(E);
            }
        });

        for (entt::entity Root : Roots)
        {
            SPrefabInstanceComponent* Inst = WorldRegistry.try_get<SPrefabInstanceComponent>(Root);
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

            entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);

            TVector<entt::entity> Roots;
            WorldRegistry.view<SPrefabInstanceComponent>().each([&](entt::entity E, const SPrefabInstanceComponent& Inst)
            {
                if (Inst.bIsRoot && Inst.SourcePrefab.Get() == this)
                {
                    Roots.push_back(E);
                }
            });

            for (entt::entity Root : Roots)
            {
                RefreshInstance(World, Root);
            }
        }
    }

    void CPrefab::CullOrphanedInstances(entt::registry& Registry)
    {
        // SourcePrefab resolves to null or to a marked-destroy zombie, and either way the entity is garbage.
        TVector<entt::entity> Orphans;
        Registry.view<SPrefabInstanceComponent>().each([&](entt::entity E, const SPrefabInstanceComponent& Inst)
        {
            CPrefab* Src = Inst.SourcePrefab.Get();
            if (Src == nullptr || Src->HasAnyFlag(OF_MarkedDestroy))
            {
                Orphans.push_back(E);
            }
        });
        for (entt::entity E : Orphans)
        {
            if (Registry.valid(E))
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

            entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);

            // A detached subtree has no instance component, so it is not matched and survives.
            TVector<entt::entity> Matching;
            WorldRegistry.view<SPrefabInstanceComponent>().each([&](entt::entity E, const SPrefabInstanceComponent& Inst)
            {
                if (Inst.SourcePrefab.Get() == this)
                {
                    Matching.push_back(E);
                }
            });

            // Entries destroyed by an earlier iteration are skipped by the validity check, so no link dangles.
            for (entt::entity E : Matching)
            {
                if (WorldRegistry.valid(E))
                {
                    ECS::Utils::DestroyEntityHierarchy(WorldRegistry, E);
                }
            }
        }
    }

    bool CPrefab::DetachInstance(CWorld* World, entt::entity InstanceRoot)
    {
        if (World == nullptr)
        {
            return false;
        }

        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*World);
        if (!WorldRegistry.valid(InstanceRoot))
        {
            return false;
        }

        const SPrefabInstanceComponent* RootInstance = WorldRegistry.try_get<SPrefabInstanceComponent>(InstanceRoot);
        if (RootInstance == nullptr || !RootInstance->bIsRoot)
        {
            return false;
        }

        // Leaves plain entities behind, so world load no longer refreshes them against the source asset.
        TVector<entt::entity> ToStrip;
        ToStrip.reserve(16);
        ToStrip.push_back(InstanceRoot);
        ECS::Utils::ForEachDescendant(WorldRegistry, InstanceRoot, [&](entt::entity Desc)
        {
            if (WorldRegistry.any_of<SPrefabInstanceComponent>(Desc))
            {
                ToStrip.push_back(Desc);
            }
        });

        for (entt::entity E : ToStrip)
        {
            WorldRegistry.remove<SPrefabInstanceComponent>(E);
        }

        // The override ledger (root-only) is meaningless once detached.
        WorldRegistry.remove<SPrefabOverrideComponent>(InstanceRoot);
        return true;
    }

    void CPrefab::CaptureFromWorld(CWorld* SourceWorld, entt::entity RootEntity)
    {
        if (SourceWorld == nullptr)
        {
            return;
        }

        entt::registry& WorldRegistry = ECS::GetWorldRegistry(*SourceWorld);
        if (!WorldRegistry.valid(RootEntity))
        {
            return;
        }

        TVector<entt::entity> EntitiesToCapture;
        EntitiesToCapture.reserve(16);
        EntitiesToCapture.push_back(RootEntity);
        ECS::Utils::ForEachDescendant(WorldRegistry, RootEntity, [&](entt::entity E)
        {
            EntitiesToCapture.push_back(E);
        });

        // CopyRegistry remaps hierarchy and handle fields, and skips nested instance tracking.
        BumpDataGeneration();
        Registry = entt::registry{};
        THashMap<entt::entity, entt::entity> Map;
        CopyRegistry(WorldRegistry, Registry, Map, &EntitiesToCapture, &ShouldSkipInstanceComponent);

        // Reuses an existing instance tag when present, so RefreshInstance can match placed counterparts.
        for (entt::entity SrcE : EntitiesToCapture)
        {
            auto It = Map.find(SrcE);
            if (It == Map.end())
            {
                continue;
            }

            FName StableID;
            if (const SPrefabInstanceComponent* Inst = WorldRegistry.try_get<SPrefabInstanceComponent>(SrcE))
            {
                StableID = Inst->StableID;
            }
            if (StableID.IsNone())
            {
                StableID = GenerateStableID();
            }
            Registry.emplace_or_replace<SPrefabComponent>(It->second).StableID = StableID;
        }

        if (CPackage* Package = GetPackage())
        {
            Package->MarkDirty();
        }
    }

    namespace
    {
        THashMap<FName, entt::entity> IndexByStableID(entt::registry& Registry)
        {
            THashMap<FName, entt::entity> Out;
            Registry.view<SPrefabComponent>().each([&](entt::entity E, const SPrefabComponent& Comp)
            {
                if (!Comp.StableID.IsNone())
                {
                    Out.try_emplace(Comp.StableID, E);
                }
            });
            return Out;
        }

        FName StableIDOf(entt::registry& Registry, entt::entity E)
        {
            const SPrefabComponent* Comp = Registry.valid(E) ? Registry.try_get<SPrefabComponent>(E) : nullptr;
            return Comp != nullptr ? Comp->StableID : FName();
        }

        FName ParentStableIDOf(entt::registry& Registry, entt::entity E)
        {
            const FRelationshipComponent* Rel = Registry.valid(E) ? Registry.try_get<FRelationshipComponent>(E) : nullptr;
            if (Rel == nullptr || Rel->Parent == entt::null)
            {
                return FName();
            }
            return StableIDOf(Registry, Rel->Parent);
        }

        CStruct* StructOfStorage(const entt::sparse_set& Storage)
        {
            using namespace entt::literals;
            entt::meta_type MetaType = entt::resolve(Storage.info());
            if (!MetaType)
            {
                return nullptr;
            }
            if (entt::meta_any S = ECS::Utils::InvokeMetaFunc(MetaType, "static_struct"_hs))
            {
                return S.cast<CStruct*>();
            }
            return nullptr;
        }

        // The delta records these separately by StableID, so they are never diffed as component data.
        bool IsStructuralStorage(entt::id_type ID)
        {
            return ID == entt::type_hash<SPrefabComponent>::value()
                || ID == entt::type_hash<FRelationshipComponent>::value();
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
        VariantDelta = entt::registry{};
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

        if (Algo::Find(VisitStack.begin(), VisitStack.end(), this) != VisitStack.end())
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

        Registry = entt::registry{};
        THashMap<entt::entity, entt::entity> Map;
        CopyRegistry(Parent->Registry, Registry, Map);

        ApplyVariantDelta();
    }

    void CPrefab::ApplyVariantDelta()
    {
        using namespace entt::literals;

        THashMap<FName, entt::entity> Resolved = IndexByStableID(Registry);

        // Surviving children are rescued to the root first, exactly as an instance refresh does.
        for (const FName& DeadID : VariantRemovedEntities)
        {
            auto It = Resolved.find(DeadID);
            if (It == Resolved.end() || !Registry.valid(It->second))
            {
                continue;
            }

            const entt::entity Dead = It->second;
            TVector<entt::entity> Survivors;
            ECS::Utils::ForEachDescendant(Registry, Dead, [&](entt::entity Desc)
            {
                const FName DescID = StableIDOf(Registry, Desc);
                if (DescID.IsNone() || Algo::Find(VariantRemovedEntities.begin(), VariantRemovedEntities.end(), DescID) == VariantRemovedEntities.end())
                {
                    Survivors.push_back(Desc);
                }
            });
            for (entt::entity S : Survivors)
            {
                if (Registry.valid(S) && Registry.all_of<STransformComponent>(S))
                {
                    ECS::Utils::ReparentEntity(Registry, S, entt::null);
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
        THashMap<entt::entity, entt::entity> DeltaToResolved;
        VariantDelta.view<SPrefabComponent>().each([&](entt::entity DeltaE, const SPrefabComponent& Comp)
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

            const entt::entity NewE = Registry.create();
            Registry.emplace<SPrefabComponent>(NewE).StableID = Comp.StableID;
            Resolved[Comp.StableID] = NewE;
            DeltaToResolved[DeltaE] = NewE;
        });

        // A component absent from the resolved data is emplaced wholesale, otherwise only its leaves land.
        VariantDelta.view<SPrefabComponent>().each([&](entt::entity DeltaE, const SPrefabComponent& Comp)
        {
            auto DestIt = DeltaToResolved.find(DeltaE);
            if (DestIt == DeltaToResolved.end() || !Registry.valid(DestIt->second))
            {
                return;
            }

            const entt::entity DestE = DestIt->second;
            const THashMap<FName, THashSet<FName>>* NodeOverrides = nullptr;
            if (auto OIt = OverridesByNode.find(Comp.StableID); OIt != OverridesByNode.end())
            {
                NodeOverrides = &OIt->second;
            }

            for (auto&& [ID, DeltaStorage] : VariantDelta.storage())
            {
                if (IsStructuralStorage(ID) || IsNonReplicatedStorage(ID)) continue;
                if (!DeltaStorage.contains(DeltaE)) continue;

                entt::meta_type MetaType = entt::resolve(DeltaStorage.info());
                if (!MetaType) continue;

                void* SrcPtr = DeltaStorage.value(DeltaE);
                CStruct* CompStruct = StructOfStorage(DeltaStorage);

                void* DstPtr = nullptr;
                if (auto* DestStorage = Registry.storage(ID); DestStorage != nullptr && DestStorage->contains(DestE))
                {
                    DstPtr = DestStorage->value(DestE);
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
                    entt::meta_any SrcAny = MetaType.from_void(SrcPtr);
                    ECS::Utils::InvokeMetaFunc(MetaType, "emplace"_hs,
                        entt::forward_as_meta(Registry), DestE, entt::forward_as_meta(SrcAny));
                }
                else
                {
                    PrefabOverride::ApplyOverriddenLeaves(CompStruct, DstPtr, SrcPtr, *Leaves);
                }
            }
        });

        // Components the variant deletes from an inherited node.
        for (auto& [NodeID, CompNames] : RemovedByNode)
        {
            auto It = Resolved.find(NodeID);
            if (It == Resolved.end() || !Registry.valid(It->second))
            {
                continue;
            }

            TVector<entt::id_type> ToRemove;
            for (auto&& [ID, Storage] : Registry.storage())
            {
                if (IsStructuralStorage(ID) || !Storage.contains(It->second)) continue;

                CStruct* CompStruct = StructOfStorage(Storage);
                if (CompStruct != nullptr && CompNames.find(CompStruct->GetName()) != CompNames.end())
                {
                    ToRemove.push_back(ID);
                }
            }

            for (entt::id_type ID : ToRemove)
            {
                if (auto* Storage = Registry.storage(ID))
                {
                    Storage->remove(It->second);
                }
            }
        }

        // Parentage last, since every node the delta addresses now exists.
        for (const SPrefabVariantNode& Node : VariantStructuralNodes)
        {
            auto ChildIt = Resolved.find(Node.StableID);
            if (ChildIt == Resolved.end() || !Registry.valid(ChildIt->second))
            {
                continue;
            }

            // ReparentEntity reads the child's transform, and an added node may not have carried one.
            if (!Registry.all_of<STransformComponent>(ChildIt->second))
            {
                Registry.emplace<STransformComponent>(ChildIt->second);
            }

            if (Node.ParentStableID.IsNone())
            {
                ECS::Utils::ReparentEntity(Registry, ChildIt->second, entt::null);
                continue;
            }

            auto ParentIt = Resolved.find(Node.ParentStableID);
            if (ParentIt != Resolved.end() && Registry.valid(ParentIt->second))
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

        THashMap<FName, entt::entity> ParentByID = IndexByStableID(Parent->Registry);
        THashMap<FName, entt::entity> MineByID   = IndexByStableID(Registry);

        // Entities the parent still has and this variant dropped.
        for (auto& [StableID, ParentE] : ParentByID)
        {
            if (MineByID.find(StableID) == MineByID.end())
            {
                VariantRemovedEntities.push_back(StableID);
            }
        }

        // Collected first so the delta registry is built in one copy.
        TVector<entt::entity> DivergedEntities;
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

            const entt::entity ParentE = ParentIt->second;

            if (ParentStableIDOf(Registry, MineE) != ParentStableIDOf(Parent->Registry, ParentE))
            {
                VariantStructuralNodes.push_back(SPrefabVariantNode{ StableID, ParentStableIDOf(Registry, MineE), false });
            }

            bool bNodeDiverged = false;

            // Components this variant carries, added outright or diverged on some leaf.
            for (auto&& [ID, MyStorage] : Registry.storage())
            {
                if (IsStructuralStorage(ID) || IsNonReplicatedStorage(ID)) continue;
                if (!MyStorage.contains(MineE)) continue;

                CStruct* CompStruct = StructOfStorage(MyStorage);
                if (CompStruct == nullptr) continue;

                auto* ParentStorage = Parent->Registry.storage(ID);
                const bool bParentHas = ParentStorage != nullptr && ParentStorage->contains(ParentE);

                if (!bParentHas)
                {
                    VariantAddedComponents.push_back(SPrefabComponentRef{ StableID, CompStruct->GetName() });
                    KeepComponentsByNode[StableID].insert(CompStruct->GetName());
                    bNodeDiverged = true;
                    continue;
                }

                TVector<FName> Leaves;
                PrefabOverride::CollectOverriddenLeaves(CompStruct, MyStorage.value(MineE),
                    ParentStorage->value(ParentE), Leaves);

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
            for (auto&& [ID, ParentStorage] : Parent->Registry.storage())
            {
                if (IsStructuralStorage(ID) || IsNonReplicatedStorage(ID)) continue;
                if (!ParentStorage.contains(ParentE)) continue;

                auto* MyStorage = Registry.storage(ID);
                if (MyStorage != nullptr && MyStorage->contains(MineE)) continue;

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

        THashMap<entt::entity, entt::entity> Map;
        CopyRegistry(Registry, VariantDelta, Map, &DivergedEntities);

        // The delta stores authored values only, and carrying the rest would freeze them against parent edits.
        VariantDelta.view<SPrefabComponent>().each([&](entt::entity DeltaE, const SPrefabComponent& Comp)
        {
            auto KeepIt = KeepComponentsByNode.find(Comp.StableID);
            const bool bAddedNode = Algo::FindIf(VariantStructuralNodes.begin(), VariantStructuralNodes.end(),
                [&](const SPrefabVariantNode& N) { return N.bAdded && N.StableID == Comp.StableID; }) != VariantStructuralNodes.end();

            if (bAddedNode)
            {
                return; // an added node authors everything it carries
            }

            TVector<entt::id_type> ToStrip;
            for (auto&& [ID, Storage] : VariantDelta.storage())
            {
                if (IsStructuralStorage(ID) || !Storage.contains(DeltaE)) continue;

                CStruct* CompStruct = StructOfStorage(Storage);
                const bool bKeep = CompStruct != nullptr
                    && KeepIt != KeepComponentsByNode.end()
                    && KeepIt->second.find(CompStruct->GetName()) != KeepIt->second.end();

                if (!bKeep)
                {
                    ToStrip.push_back(ID);
                }
            }

            for (entt::id_type ID : ToStrip)
            {
                if (auto* Storage = VariantDelta.storage(ID))
                {
                    Storage->remove(DeltaE);
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
        Registry.view<SPrefabComponent>().each([&](entt::entity E, const SPrefabComponent& Comp)
        {
            if (!Comp.StableID.IsNone())
            {
                // A duplicate id is a bug elsewhere, and picking the same entity keeps the baseline stable.
                StableIDLookup.try_emplace(Comp.StableID, E);
            }
        });
    }

    entt::entity CPrefab::FindEntityByStableID(const FName& StableID)
    {
        if (StableID.IsNone())
        {
            return entt::null;
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
            return entt::null;
        }

        // A missed generation bump would hand back a recycled entity, and a wrong component is worse than none.
        const entt::entity Cached = It->second;
        if (!Registry.valid(Cached))
        {
            return entt::null;
        }

        const SPrefabComponent* Comp = Registry.try_get<SPrefabComponent>(Cached);
        return (Comp != nullptr && Comp->StableID == StableID) ? Cached : entt::null;
    }

    void* CPrefab::ResolvePrefabComponentPtr(const FName& StableID, CStruct* Struct)
    {
        if (Struct == nullptr || StableID.IsNone())
        {
            return nullptr;
        }

        const entt::entity PrefabE = FindEntityByStableID(StableID);
        if (PrefabE == entt::null)
        {
            return nullptr;
        }

        return FindReflectedComponentPtr(Registry, PrefabE, Struct);
    }

    entt::entity CPrefab::FindInstanceRoot(entt::registry& Registry, entt::entity Entity)
    {
        entt::entity Cur = Entity;
        while (Registry.valid(Cur))
        {
            const SPrefabInstanceComponent* Inst = Registry.try_get<SPrefabInstanceComponent>(Cur);
            if (Inst == nullptr)
            {
                return entt::null;
            }
            if (Inst->bIsRoot)
            {
                return Cur;
            }
            const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Cur);
            Cur = Rel ? Rel->Parent : entt::null;
        }
        return entt::null;
    }

    void CPrefab::RecaptureComponentOverrides(entt::registry& Registry, entt::entity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || !Registry.valid(Entity))
        {
            return;
        }

        const SPrefabInstanceComponent* Inst = Registry.try_get<SPrefabInstanceComponent>(Entity);
        if (Inst == nullptr || Inst->SourcePrefab == nullptr)
        {
            return;
        }

        const entt::entity Root = FindInstanceRoot(Registry, Entity);
        if (Root == entt::null)
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

        SPrefabOverrideComponent& Ledger = Registry.get_or_emplace<SPrefabOverrideComponent>(Root);

        // Replace this (node, component) pair's records with the freshly computed set.
        auto& Recs = Ledger.PropertyOverrides;
        Recs.erase(Algo::RemoveIf(Recs.begin(), Recs.end(), [&](const SPrefabPropertyOverride& O)
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

    void CPrefab::NoteComponentAdded(entt::registry& Registry, entt::entity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || !Registry.valid(Entity))
        {
            return;
        }

        const SPrefabInstanceComponent* Inst = Registry.try_get<SPrefabInstanceComponent>(Entity);
        if (Inst == nullptr)
        {
            return;
        }
        const entt::entity Root = FindInstanceRoot(Registry, Entity);
        if (Root == entt::null)
        {
            return;
        }

        const FName NodeID = Inst->StableID;
        const FName CompName = ComponentType->GetName();
        const bool bPrefabHas = Inst->SourcePrefab != nullptr
            && Inst->SourcePrefab->ResolvePrefabComponentPtr(NodeID, ComponentType) != nullptr;

        SPrefabOverrideComponent& Ledger = Registry.get_or_emplace<SPrefabOverrideComponent>(Root);

        auto MatchesPair = [&](const SPrefabComponentRef& C)
        {
            return C.EntityStableID == NodeID && C.ComponentType == CompName;
        };

        // A genuinely new component is recorded as instance-added, so refresh never prunes it.
        auto& Removed = Ledger.RemovedComponents;
        Removed.erase(Algo::RemoveIf(Removed.begin(), Removed.end(), MatchesPair), Removed.end());

        if (!bPrefabHas)
        {
            auto& Added = Ledger.AddedComponents;
            if (Algo::FindIf(Added.begin(), Added.end(), MatchesPair) == Added.end())
            {
                SPrefabComponentRef Rec;
                Rec.EntityStableID = NodeID;
                Rec.ComponentType  = CompName;
                Added.push_back(Rec);
            }
        }
    }

    void CPrefab::NoteComponentRemoved(entt::registry& Registry, entt::entity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || !Registry.valid(Entity))
        {
            return;
        }

        const SPrefabInstanceComponent* Inst = Registry.try_get<SPrefabInstanceComponent>(Entity);
        if (Inst == nullptr)
        {
            return;
        }
        const entt::entity Root = FindInstanceRoot(Registry, Entity);
        if (Root == entt::null)
        {
            return;
        }

        const FName NodeID = Inst->StableID;
        const FName CompName = ComponentType->GetName();
        const bool bPrefabHas = Inst->SourcePrefab != nullptr
            && Inst->SourcePrefab->ResolvePrefabComponentPtr(NodeID, ComponentType) != nullptr;

        SPrefabOverrideComponent& Ledger = Registry.get_or_emplace<SPrefabOverrideComponent>(Root);

        auto MatchesPair = [&](const auto& C)
        {
            return C.EntityStableID == NodeID && C.ComponentType == CompName;
        };

        // Any property overrides for the gone component are meaningless now.
        auto& Props = Ledger.PropertyOverrides;
        Props.erase(Algo::RemoveIf(Props.begin(), Props.end(), MatchesPair), Props.end());

        auto& Added = Ledger.AddedComponents;
        Added.erase(Algo::RemoveIf(Added.begin(), Added.end(), MatchesPair), Added.end());

        auto& Removed = Ledger.RemovedComponents;
        Removed.erase(Algo::RemoveIf(Removed.begin(), Removed.end(), MatchesPair), Removed.end());

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
