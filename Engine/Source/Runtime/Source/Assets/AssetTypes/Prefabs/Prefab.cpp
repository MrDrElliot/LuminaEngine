#include "RuntimePCH.h"

#include "World/ECS/ECSReferenceProvider.h"
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
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/TagComponent.h"
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

        TFunction<void(CWorld*, bool)> GLoadedWorldRefreshHook;

        // Nested instance tracking must not leak into the new prefab, which gets fresh tags instead.
        bool ShouldSkipInstanceComponent(uint32 ID)
        {
            return CPrefab::IsInstanceTrackingComponent(ID);
        }
        
        CClass* AsScriptClass(CStruct* Type)
        {
            if (Type == nullptr || !Type->IsA<CClass>())
            {
                return nullptr;
            }
            CClass* Class = static_cast<CClass*>(Type);
            return Class->IsChildOf(CEntityScript::StaticClass()) ? Class : nullptr;
        }

        // The ledger and instance tag describe the placement itself, so editing them is never an override.
        bool IsInstanceTrackingStruct(const CStruct* Type)
        {
            return Type == SPrefabInstanceComponent::StaticStruct() || Type == SPrefabOverrideComponent::StaticStruct();
        }

        CEntityScript* FindScriptOfClass(const SEntityScriptComponent& Component, const CClass* Class)
        {
            for (const TStrongObjectPtr<CEntityScript>& Held : Component.Scripts)
            {
                if (CEntityScript* Script = Held.Get(); Script != nullptr && Script->GetClass() == Class)
                {
                    return Script;
                }
            }
            return nullptr;
        }

        // A script field's override path is its class name then the field, so scripts on one entity never collide.
        FString ScriptPathPrefix(const CClass* Class)
        {
            return FString(*Class->GetName().ToString()) + ".";
        }

        // Which scripts an instance carries is an override too, marked per class beside its field paths.
        constexpr const char* ScriptRemovedMarker = "$Removed";
        constexpr const char* ScriptAddedMarker   = "$Added";

        void CollectScriptOverrides(const SEntityScriptComponent& Instance, const SEntityScriptComponent& Prefab, TVector<FName>& OutPaths)
        {
            for (const TStrongObjectPtr<CEntityScript>& Held : Prefab.Scripts)
            {
                if (CEntityScript* PrefabScript = Held.Get(); PrefabScript != nullptr && FindScriptOfClass(Instance, PrefabScript->GetClass()) == nullptr)
                {
                    OutPaths.Emplace(*(ScriptPathPrefix(PrefabScript->GetClass()) + ScriptRemovedMarker));
                }
            }

            for (const TStrongObjectPtr<CEntityScript>& Held : Instance.Scripts)
            {
                CEntityScript* InstanceScript = Held.Get();
                CEntityScript* PrefabScript = InstanceScript != nullptr ? FindScriptOfClass(Prefab, InstanceScript->GetClass()) : nullptr;
                if (PrefabScript == nullptr)
                {
                    if (InstanceScript != nullptr)
                    {
                        OutPaths.Emplace(*(ScriptPathPrefix(InstanceScript->GetClass()) + ScriptAddedMarker));
                    }
                    continue;
                }

                TVector<FName> Leaves;
                PrefabOverride::CollectOverriddenLeaves(InstanceScript->GetClass(), InstanceScript, PrefabScript, Leaves);
                const FString Prefix = ScriptPathPrefix(InstanceScript->GetClass());
                for (const FName& Leaf : Leaves)
                {
                    OutPaths.Emplace(*(Prefix + *Leaf));
                }
            }
        }

        bool IsScriptMarked(const THashSet<FName>& OverriddenPaths, const CClass* Class, const char* Marker)
        {
            return OverriddenPaths.Contains(FName(*(ScriptPathPrefix(Class) + Marker)));
        }

        // The overridden field paths of one script class, with the class prefix stripped.
        THashSet<FName> ScriptClassPaths(const THashSet<FName>& OverriddenPaths, const CClass* Class)
        {
            const FString Prefix = ScriptPathPrefix(Class);
            THashSet<FName> ClassPaths;
            for (const FName& Path : OverriddenPaths)
            {
                const FStringView Full(*Path);
                if (Full.StartsWith(FStringView(Prefix)))
                {
                    ClassPaths.Emplace(*FString(Full.Mid(Prefix.Len())));
                }
            }
            return ClassPaths;
        }

        // The variant side of ApplyInheritedScriptLeaves, writing only what the variant authored onto inherited script data.
        void ApplyOverriddenScriptLeaves(SEntityScriptComponent& Dest, const SEntityScriptComponent& Authored, const THashSet<FName>& OverriddenPaths)
        {
            Dest.Scripts.RemoveAll([&](const TStrongObjectPtr<CEntityScript>& Held)
            {
                return Held.Get() != nullptr && IsScriptMarked(OverriddenPaths, Held->GetClass(), ScriptRemovedMarker);
            });

            for (const TStrongObjectPtr<CEntityScript>& Held : Authored.Scripts)
            {
                CEntityScript* AuthoredScript = Held.Get();
                if (AuthoredScript == nullptr)
                {
                    continue;
                }

                CClass* Class = AuthoredScript->GetClass();
                const bool bAdded = IsScriptMarked(OverriddenPaths, Class, ScriptAddedMarker);
                CEntityScript* DestScript = FindScriptOfClass(Dest, Class);
                if (DestScript == nullptr)
                {
                    if (bAdded)
                    {
                        if (CEntityScript* Clone = EntityScripts::Clone(AuthoredScript))
                        {
                            Dest.Scripts.Add(Clone);
                        }
                    }
                    continue;
                }

                // An added script authors every field, even once the parent ships that class too.
                if (bAdded)
                {
                    PrefabOverride::ApplyInheritedLeaves(Class, DestScript, AuthoredScript, {});
                }
                else
                {
                    PrefabOverride::ApplyOverriddenLeaves(Class, DestScript, AuthoredScript, ScriptClassPaths(OverriddenPaths, Class));
                }
            }
        }

        // In place, so a live script keeps running and only the fields nobody overrode follow the prefab.
        void ApplyInheritedScriptLeaves(ECS::FRegistry& Registry, ECS::FEntity Entity, const SEntityScriptComponent& Prefab, const THashSet<FName>& OverriddenPaths,
            const TVector<const CClass*>* PreviouslyInherited)
        {
            for (const TStrongObjectPtr<CEntityScript>& Held : Prefab.Scripts)
            {
                CEntityScript* PrefabScript = Held.Get();
                if (PrefabScript == nullptr)
                {
                    continue;
                }

                CClass* Class = PrefabScript->GetClass();
                const SEntityScriptComponent* Current = Registry.TryGet<SEntityScriptComponent>(Entity);
                CEntityScript* InstanceScript = Current != nullptr ? FindScriptOfClass(*Current, Class) : nullptr;
                if (InstanceScript == nullptr)
                {
                    if (OverriddenPaths.Contains(FName(*(ScriptPathPrefix(Class) + ScriptRemovedMarker))))
                    {
                        continue;
                    }
                    InstanceScript = EntityScripts::Attach(Registry, Entity, Class);
                    if (InstanceScript == nullptr)
                    {
                        continue;
                    }
                }

                PrefabOverride::ApplyInheritedLeaves(Class, InstanceScript, PrefabScript, ScriptClassPaths(OverriddenPaths, Class));
            }

            // A script the prefab dropped goes too, unless the instance added that class itself.
            TVector<CEntityScript*> Dropped;
            const SEntityScriptComponent* Current = PreviouslyInherited != nullptr ? Registry.TryGet<SEntityScriptComponent>(Entity) : nullptr;
            if (Current != nullptr)
            {
                for (const TStrongObjectPtr<CEntityScript>& Held : Current->Scripts)
                {
                    CEntityScript* InstanceScript = Held.Get();
                    if (InstanceScript == nullptr || FindScriptOfClass(Prefab, InstanceScript->GetClass()) != nullptr
                        || !Algo::Contains(*PreviouslyInherited, InstanceScript->GetClass()))
                    {
                        continue;
                    }
                    const FName AddedPath(*(ScriptPathPrefix(InstanceScript->GetClass()) + ScriptAddedMarker));
                    if (!OverriddenPaths.Contains(AddedPath))
                    {
                        Dropped.Add(InstanceScript);
                    }
                }
            }
            for (CEntityScript* Script : Dropped)
            {
                EntityScripts::Remove(Registry, Entity, Script);
            }
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

        // A nested instance root begins a placement of its own, so neither it nor anything below it is this instance's node.
        template<typename TFunc>
        void ForEachOwnInstanceNode(ECS::FRegistry& Registry, ECS::FEntity InstanceRoot, TFunc&& Func)
        {
            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            TVector<ECS::FEntity> Pending;
            auto PushChildren = [&](ECS::FEntity Parent)
            {
                for (ECS::FEntity Child = Hierarchy.GetLastChild(Parent); !Child.IsNull(); Child = Hierarchy.GetPrevSibling(Child))
                {
                    const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Child);
                    if (Inst == nullptr || !Inst->bIsRoot)
                    {
                        Pending.Add(Child);
                    }
                }
            };
            PushChildren(InstanceRoot);
            while (!Pending.IsEmpty())
            {
                const ECS::FEntity Current = Pending.Last();
                Pending.Pop();
                Func(Current);
                PushChildren(Current);
            }
        }

        // Captures before nested placements were told apart saved repeated ids, and a derived id is the same on every load.
        void RemintRepeatedStableIDs(ECS::FRegistry& Registry, const CPrefab& Prefab)
        {
            THashMap<FName, int32> Seen;
            int32 Reminted = 0;
            Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity, SPrefabComponent& Node)
            {
                if (Node.StableID.IsNone())
                {
                    return;
                }
                const int32 Repeat = Seen[Node.StableID]++;
                if (Repeat > 0)
                {
                    Node.StableID = FName(Format("{}_{}", *Node.StableID, Repeat));
                    ++Reminted;
                }
            });
            if (Reminted > 0)
            {
                LOG_WARN("Prefab '{}' repeated {} node id(s); they were given their own, so resave it to keep them.", *Prefab.GetName(), Reminted);
            }
        }

        // A prefab value moved into one instance's ids, so leaves taken from it land remapped and overridden leaves keep their own handles.
        class FRemappedPrefabValue
        {
        public:

            FRemappedPrefabValue(CStruct* InStruct, const void* Source, const THashMap<ECS::FEntity, ECS::FEntity>& Map)
                : Struct(InStruct)
                , Value(Memory::Malloc(InStruct->GetSize(), InStruct->GetAlignment()))
            {
                Struct->InitializeStruct(Value);
                Struct->CopyStruct(Value, Source);
                ECS::Utils::RemapEntityReferences(Struct, Value, Map, /*bClearUnmapped*/ true);
            }

            ~FRemappedPrefabValue()
            {
                Struct->DestroyStruct(Value);
                Memory::Free(Value);
            }

            LE_NO_COPYMOVE(FRemappedPrefabValue);

            void* Get() const { return Value; }

        private:

            CStruct* Struct;
            void* Value;
        };

        // The prefab's entities keyed to one placement's own nodes, so a prefab value can be read in that placement's ids.
        THashMap<ECS::FEntity, ECS::FEntity> MapPrefabOntoInstance(CPrefab& Prefab, ECS::FRegistry& Registry, ECS::FEntity Root)
        {
            THashMap<ECS::FEntity, ECS::FEntity> Map;
            auto Add = [&](ECS::FEntity Entity)
            {
                const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Entity);
                if (Inst == nullptr || Inst->SourcePrefab.Get() != &Prefab)
                {
                    return;
                }
                if (const ECS::FEntity PrefabEntity = Prefab.FindEntityByStableID(Inst->StableID); PrefabEntity != ECS::NullEntity)
                {
                    Map[PrefabEntity] = Entity;
                }
            };
            Add(Root);
            ForEachOwnInstanceNode(Registry, Root, Add);
            return Map;
        }

        // Otherwise freshly spawned entities render at a stale position for one frame.
        void MarkSubtreeTransformsDirty(ECS::FRegistry& Registry, ECS::FEntity Root)
        {
            if (!Registry.IsValid(Root))
            {
                return;
            }
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Root);
            Registry.GetHierarchy().ForEachDescendant(Root, [&](ECS::FEntity Desc)
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

    void CPrefab::MarkRegistryChanged()
    {
        // Drawn from the process-wide generation, so a prefab freed and loaded again never repeats a revision a copy was stamped with.
        BumpDataGeneration();
        Revision = GetDataGeneration();
    }

    void CPrefab::BumpDataGeneration()
    {
        ++GDataGeneration;
    }

    void CPrefab::VisitAdditionalObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
    {
        VisitRegistryObjectReferences(Registry, Func);
        VisitRegistryObjectReferences(VariantDelta, Func);
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

        if (Ar.IsReading())
        {
            RemintRepeatedStableIDs(bVariantPayload ? VariantDelta : Registry, *this);
            MarkRegistryChanged();
        }
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
                *GetName());
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
                auto It = OutMap.Find(SrcE);
                if (It == nullptr)
                {
                    continue;
                }

                ECS::FEntity DestE = (*It);
                void* SrcCompPtr = SrcSet.GetRaw(SrcE);

                // Each tag has its own storage, and copied by type alone every tag of an entity would land in one unnamed slot.
                if (ID == ECS::GetComponentTypeID<STagComponent>())
                {
                    const STagComponent& Tag = *static_cast<const STagComponent*>(SrcCompPtr);
                    Dest.NamedStorage<STagComponent>(Tag.Tag).EmplaceOrReplace(DestE, Tag);
                    continue;
                }

                Ops->EmplaceCopy(Dest, DestE, SrcCompPtr);
            }
        }

        // Copying the live body id makes the physics scene skip creation as already existing.
        for (auto& [SrcE, DestE] : OutMap)
        {
            if (const SRigidBodyComponent* SrcBody = Source.TryGet<SRigidBodyComponent>(SrcE))
            {
                SRigidBodyComponent NewBody = *SrcBody;
                Dest.EmplaceOrReplace<SRigidBodyComponent>(DestE, NewBody);
            }
        }

        // A link whose parent was not copied is dropped, so that child becomes a root of the copy.
        const ECS::FHierarchy& SourceHierarchy = Source.GetHierarchy();
        TVector<uint32> SiblingIndices;
        SourceHierarchy.BuildSiblingIndices(SiblingIndices);

        ECS::Utils::FParentLinks Links;
        for (const auto& [SrcE, DestE] : OutMap)
        {
            const auto ParentIt = OutMap.Find(SourceHierarchy.GetParent(SrcE));
            if (ParentIt != nullptr)
            {
                Links.Links.Emplace(DestE, (*ParentIt), SiblingIndices[SrcE.GetIndex()]);
            }
        }
        Links.Apply(Dest);

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
            LOG_WARN("Prefab '{}' did not resolve against its parent; refusing to instantiate it.", *GetName());
            return ECS::NullEntity;
        }

        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& WorldRegistry = TargetWorld->GetEntityRegistry();
        
        TVector<ECS::FEntity> PrefabRoots;
        PrefabRoots.Reserve(2);
        Registry.ForEachEntity([&](ECS::FEntity E)
        {
            if (Registry.GetHierarchy().GetParent(E) == ECS::NullEntity)
            {
                PrefabRoots.Add(E);
            }
        });

        if (PrefabRoots.IsEmpty())
        {
            LOG_WARN("Prefab '{}' has no entities; nothing to instantiate.", *GetName());
            return ECS::NullEntity;
        }

        const ECS::FEntity PrefabRoot = PrefabRoots[0];
        if (PrefabRoots.Num() > 1)
        {
            LOG_WARN("Prefab '{}' has {} parentless entities; reparenting extras under the first.",
                     *GetName(), (uint32)PrefabRoots.Num());
        }
        
        THashMap<ECS::FEntity, ECS::FEntity> Map;
        ECS::FEntity WorldRoot = ECS::NullEntity;

        {
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
                Instance.AppliedRevision = Revision;
            }

            // Rescue any extra parentless entities so the spawn has a single hierarchical root.
            for (int32 i = 1; i < PrefabRoots.Num(); ++i)
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

    void CPrefab::CapturePreCommitState()
    {
        ClearPreCommitState();
        Registry.View<SPrefabComponent, STransformComponent>().ForEach(
            [&](ECS::FEntity, const SPrefabComponent& Node, const STransformComponent& Transform)
        {
            PreCommitTransforms[Node.StableID] = Transform.LocalTransform;
        });
        Registry.View<SPrefabComponent, SEntityScriptComponent>().ForEach(
            [&](ECS::FEntity, const SPrefabComponent& Node, const SEntityScriptComponent& Scripts)
        {
            TVector<const CClass*>& Classes = PreCommitScripts[Node.StableID];
            for (const TStrongObjectPtr<CEntityScript>& Held : Scripts.Scripts)
            {
                if (const CEntityScript* Script = Held.Get())
                {
                    Classes.Add(Script->GetClass());
                }
            }
        });
    }

    void CPrefab::ClearPreCommitState()
    {
        PreCommitTransforms.Reset();
        PreCommitScripts.Reset();
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
                *GetName());
            return;
        }

        // Same hazard from any other cause, and a refusal costs a stale instance where a diff costs the placement.
        if (Registry.NumEntities() == 0)
        {
            LOG_WARN("Prefab '{}' has no entities; leaving its instances untouched rather than emptying them.",
                *GetName());
            return;
        }

        ECS::FRegistry& WorldRegistry = World->GetEntityRegistry();
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
        ForEachOwnInstanceNode(WorldRegistry, InstanceRoot, [&](ECS::FEntity Descendant)
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

        // Decided before anything moves, since every reparent below can rewrite the locals this compares.
        THashSet<ECS::FEntity> Followers;
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            auto PrefabIt = PrefabByStableID.Find(StableID);
            if (WorldE == InstanceRoot || PrefabIt == nullptr)
            {
                continue;
            }
            const STransformComponent* Authored = Registry.TryGet<STransformComponent>(*PrefabIt);
            const STransformComponent* Placed = WorldRegistry.TryGet<STransformComponent>(WorldE);
            if (Authored == nullptr || Placed == nullptr)
            {
                continue;
            }
            auto Previous = PreCommitTransforms.Find(StableID);
            const bool bUntouched = Previous != nullptr && Placed->LocalTransform == (*Previous);
            if (bUntouched || Placed->LocalTransform == Authored->LocalTransform)
            {
                Followers.Add(WorldE);
            }
        }

        // Destroy instance entities whose prefab counterpart is gone (never the user-placed root).
        TVector<ECS::FEntity> ToDestroy;
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (WorldE == InstanceRoot)
            {
                continue;
            }
            if (!PrefabByStableID.Contains(StableID))
            {
                ToDestroy.Add(WorldE);
            }
        }
        // Survivors detach to the root first, and the hierarchy-mirror pass re-nests them per the prefab.
        THashSet<ECS::FEntity> DeadSet;
        DeadSet.Reserve(ToDestroy.Num());
        for (ECS::FEntity E : ToDestroy)
        {
            DeadSet.Add(E);
        }
        for (ECS::FEntity Dead : ToDestroy)
        {
            if (!WorldRegistry.IsValid(Dead))
            {
                continue;
            }
            TVector<ECS::FEntity> Survivors;
            WorldRegistry.GetHierarchy().ForEachDescendant(Dead, [&](ECS::FEntity Desc)
            {
                if (!DeadSet.Contains(Desc))
                {
                    Survivors.Add(Desc);
                }
            });
            for (ECS::FEntity S : Survivors)
            {
                if (WorldRegistry.IsValid(S) && WorldRegistry.HasAll<STransformComponent>(S))
                {
                    ECS::Utils::ReparentEntity(WorldRegistry, S, InstanceRoot, !Followers.Contains(S));
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
                StaleIDs.Add(StableID);
            }
        }
        for (const FName& StaleID : StaleIDs)
        {
            InstanceByStableID.Remove(StaleID);
        }

        THashSet<FName> RemovedNodes;
        if (SPrefabOverrideComponent* Ledger = WorldRegistry.TryGet<SPrefabOverrideComponent>(InstanceRoot))
        {
            // A node the prefab dropped, or one an undo brought back, no longer needs suppressing.
            auto& Removed = Ledger->RemovedEntities;
            Removed.RemoveAll([&](const FName& NodeID)
            {
                return !PrefabByStableID.Contains(NodeID) || InstanceByStableID.Contains(NodeID);
            });
            RemovedNodes.Append(Removed);
        }

        // A node the prefab adds beneath a deleted one goes with it, rather than surfacing under the root.
        auto IsUnderRemovedNode = [&](ECS::FEntity PrefabE)
        {
            for (ECS::FEntity Cur = PrefabE; Registry.IsValid(Cur);)
            {
                if (const SPrefabComponent* Tag = Registry.TryGet<SPrefabComponent>(Cur); Tag && RemovedNodes.Contains(Tag->StableID))
                {
                    return true;
                }
                Cur = Registry.GetHierarchy().GetParent(Cur);
            }
            return false;
        };

        // Spawn instance entities for new prefab entries.
        THashSet<ECS::FEntity> Spawned;
        for (auto& [StableID, PrefabE] : PrefabByStableID)
        {
            if (InstanceByStableID.Contains(StableID))
            {
                continue;
            }
            if (!RemovedNodes.IsEmpty() && IsUnderRemovedNode(PrefabE))
            {
                continue;
            }
            // A recycled slot could be one the world editor's undo history means to restore.
            const ECS::FEntity NewE = WorldRegistry.CreateInFreshSlot();
            InstanceByStableID[StableID] = NewE;
            Spawned.Add(NewE);

            SPrefabInstanceComponent& Inst = WorldRegistry.Emplace<SPrefabInstanceComponent>(NewE);
            Inst.SourcePrefab = this;
            Inst.StableID     = StableID;
            Inst.bIsRoot      = false;
        }

        // Prefab-entity -> instance-entity remap table (for entity-handle fields).
        THashMap<ECS::FEntity, ECS::FEntity> PrefabToInstance;
        for (auto& [StableID, PrefabE] : PrefabByStableID)
        {
            auto It = InstanceByStableID.Find(StableID);
            if (It != nullptr)
            {
                PrefabToInstance[PrefabE] = (*It);
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
                OverriddenLeaves[O.EntityStableID][O.ComponentType].Add(O.PropertyPath);
            }
            for (const SPrefabComponentRef& C : Ledger->AddedComponents)
            {
                AddedComponents[C.EntityStableID].Add(C.ComponentType);
            }
            for (const SPrefabComponentRef& C : Ledger->RemovedComponents)
            {
                RemovedComponents[C.EntityStableID].Add(C.ComponentType);
            }
        }

        // This subsumes the old root-only case and keeps gizmo edits, which bypass the property hook.
        const uint32 TransformID = ECS::GetComponentTypeID<STransformComponent>();
        const uint32 NameID = ECS::GetComponentTypeID<SNameComponent>();
        const uint32 TagID = ECS::GetComponentTypeID<STagComponent>();
        const uint32 ScriptComponentID = ECS::GetComponentTypeID<SEntityScriptComponent>();

        // EmplaceOrReplace raises OnUpdate rather than OnDestroy, so a script's OnDetach is skipped.
        auto EmplaceFromPrefab = [&](uint32 ID, CStruct* Struct, const FComponentOps* Ops, ECS::FEntity WorldE, void* SrcCompPtr)
        {
            if (ID == ScriptComponentID)
            {
                EntityScripts::DetachAll(WorldRegistry, WorldE);
                if (!WorldRegistry.IsValid(WorldE))
                {
                    return;
                }
            }
            Ops->EmplaceCopy(WorldRegistry, WorldE, SrcCompPtr);

            // Every handle in it came from the prefab, which never points outside itself.
            ECS::Utils::RemapEntityReferences(Struct, Ops->Get(WorldRegistry, WorldE), PrefabToInstance, /*bClearUnmapped*/ true);
        };

        // Copy/replace components prefab -> instance honoring overrides, then prune ones the prefab dropped.
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity PrefabE, const SPrefabComponent& PrefabComp)
        {
            auto It = InstanceByStableID.Find(PrefabComp.StableID);
            if (It == nullptr)
            {
                return;
            }

            const ECS::FEntity WorldE = (*It);
            if (!WorldRegistry.IsValid(WorldE))
            {
                return; // stale mapping (e.g. collaterally destroyed); skip this node.
            }
            const FName NodeID = PrefabComp.StableID;

            const THashMap<FName, THashSet<FName>>* NodeOverrides = nullptr;
            if (auto NIt = OverriddenLeaves.Find(NodeID); NIt != nullptr)
            {
                NodeOverrides = &(*NIt);
            }
            const THashSet<FName>* NodeRemoved = nullptr;
            if (auto RIt = RemovedComponents.Find(NodeID); RIt != nullptr)
            {
                NodeRemoved = &(*RIt);
            }
            const THashSet<FName>* NodeAdded = nullptr;
            if (auto AIt = AddedComponents.Find(NodeID); AIt != nullptr)
            {
                NodeAdded = &(*AIt);
            }
            const bool bNodeHasLedger = (NodeOverrides != nullptr) || (NodeRemoved != nullptr);

            // Track the prefab's storages for this entity so we can drop ones the prefab no longer has.
            THashSet<uint32> PrefabComponentIDs;
            PrefabComponentIDs.Reserve(8);

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

                // Tags carry no ledger, so the prefab's are added and one the instance gave itself is kept.
                if (ID == TagID)
                {
                    const STagComponent& Tag = *static_cast<const STagComponent*>(SrcCompPtr);
                    WorldRegistry.NamedStorage<STagComponent>(Tag.Tag).EmplaceOrReplace(WorldE, Tag);
                    continue;
                }

                // A spawned entity left with no transform would crash the hierarchy-mirror reparent.
                if (ID == TransformID)
                {
                    auto* WorldXform = WorldRegistry.FindStorage(ID);
                    if (WorldXform == nullptr || !WorldXform->Contains(WorldE))
                    {
                        Ops->EmplaceCopy(WorldRegistry, WorldE, SrcCompPtr);
                    }
                    else if (WorldE != InstanceRoot)
                    {
                        // Gizmo moves record no override, so only a child still at the old prefab value follows the edit.
                        auto Previous = PreCommitTransforms.Find(NodeID);
                        const FTransform& Authored = static_cast<const STransformComponent*>(SrcCompPtr)->LocalTransform;
                        STransformComponent& Placed = WorldRegistry.Get<STransformComponent>(WorldE);
                        if (Previous != nullptr && (*Previous) != Authored && Placed.LocalTransform == (*Previous))
                        {
                            Placed.SetLocalTransform(Authored);
                        }
                    }
                    continue;
                }

                // A placed root carries the name its level gave it, so like its transform it is only ever seeded.
                if (ID == NameID && WorldE == InstanceRoot)
                {
                    PrefabComponentIDs.Add(ID);
                    if (!WorldRegistry.HasAll<SNameComponent>(WorldE))
                    {
                        Ops->EmplaceCopy(WorldRegistry, WorldE, SrcCompPtr);
                    }
                    continue;
                }

                // A node with no ledger refreshes exactly as before.
                if (!bNodeHasLedger)
                {
                    PrefabComponentIDs.Add(ID);
                    EmplaceFromPrefab(ID, CompStruct, Ops, WorldE, SrcCompPtr);
                    continue;
                }

                const FName CompName = CompStruct->GetName();

                // The instance deleted this inherited component, so leave it absent rather than re-adding.
                if (NodeRemoved && !CompName.IsNone() && NodeRemoved->Contains(CompName))
                {
                    continue;
                }

                PrefabComponentIDs.Add(ID);

                const THashSet<FName>* CompOverrides = nullptr;
                if (NodeOverrides && CompStruct)
                {
                    if (auto OIt = NodeOverrides->Find(CompName); OIt != nullptr && !OIt->IsEmpty())
                    {
                        CompOverrides = &(*OIt);
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

                if (ID == ScriptComponentID && CompOverrides != nullptr && DstCompPtr != nullptr)
                {
                    const FRemappedPrefabValue Inherited(CompStruct, SrcCompPtr, PrefabToInstance);
                    const auto PreviousScripts = PreCommitScripts.Find(NodeID);
                    ApplyInheritedScriptLeaves(WorldRegistry, WorldE, *static_cast<const SEntityScriptComponent*>(Inherited.Get()), *CompOverrides,
                        PreviousScripts != nullptr ? &(*PreviousScripts) : nullptr);
                    continue;
                }

                // Otherwise replace wholesale, which also adds a missing inherited component.
                if (CompOverrides != nullptr && DstCompPtr != nullptr)
                {
                    const FRemappedPrefabValue Inherited(CompStruct, SrcCompPtr, PrefabToInstance);
                    PrefabOverride::ApplyInheritedLeaves(CompStruct, DstCompPtr, Inherited.Get(), *CompOverrides);
                    Ops->Patch(WorldRegistry, WorldE);
                }
                else
                {
                    EmplaceFromPrefab(ID, CompStruct, Ops, WorldE, SrcCompPtr);
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
                if (ID == TagID)                   continue;
                if (PrefabComponentIDs.Contains(ID)) continue;
                if (!WorldStorage.Contains(WorldE)) continue;

                // Keep instance-added components the prefab never shipped.
                if (NodeAdded != nullptr)
                {
                    if (CStruct* WS = FindComponentStructByTypeId(ID);
                        WS != nullptr && NodeAdded->Contains(WS->GetName()))
                    {
                        continue;
                    }
                }

                ToRemoveStorages.Add(ID);
            }
            for (uint32 ID : ToRemoveStorages)
            {
                if (auto* Storage = WorldRegistry.FindStorage(ID))
                {
                    // Signaled as FRegistry::Remove does, or the render scene and physics keep the dropped component's state.
                    Storage->Signals.OnDestroy.Broadcast(WorldRegistry, WorldE);
                    Storage->RemoveEntity(WorldE);
                }
            }
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
        struct FInstanceMove
        {
            ECS::FEntity Entity;
            ECS::FEntity Parent;
            bool bPreserveWorld;
        };
        TVector<FInstanceMove> Moves;
        for (auto& [StableID, WorldE] : InstanceByStableID)
        {
            if (WorldE == InstanceRoot) continue;
            if (!WorldRegistry.IsValid(WorldE)) continue;

            const auto PrefabIt = PrefabByStableID.Find(StableID);
            if (PrefabIt == nullptr) continue;

            const ECS::FEntity PrefabParent = Registry.GetHierarchy().GetParent(*PrefabIt);

            ECS::FEntity DesiredWorldParent = InstanceRoot;
            if (PrefabParent != ECS::NullEntity)
            {
                if (const SPrefabComponent* ParentTag = Registry.TryGet<SPrefabComponent>(PrefabParent))
                {
                    auto ParentIt = InstanceByStableID.Find(ParentTag->StableID);
                    if (ParentIt != nullptr)
                    {
                        DesiredWorldParent = (*ParentIt);
                    }
                }
            }

            const ECS::FEntity CurrentParent = WorldRegistry.GetHierarchy().GetParent(WorldE);
            // ReparentEntity requires a transform on both sides, so a transform-less node would crash the refresh.
            if (CurrentParent != DesiredWorldParent
                && WorldRegistry.IsValid(DesiredWorldParent)
                && WorldRegistry.HasAll<STransformComponent>(WorldE)
                && WorldRegistry.HasAll<STransformComponent>(DesiredWorldParent))
            {
                // A node following the prefab keeps its authored local, and only one the instance moved keeps its placement.
                const bool bFollowsPrefab = Spawned.Contains(WorldE) || Followers.Contains(WorldE);
                Moves.Emplace(WorldE, DesiredWorldParent, !bFollowsPrefab);
            }
        }

        // Every mover lifts to the placed root before any lands, since a swap applied one move at a time passes through a cycle.
        for (const FInstanceMove& Move : Moves)
        {
            if (WorldRegistry.GetHierarchy().GetParent(Move.Entity) != InstanceRoot)
            {
                ECS::Utils::ReparentEntity(WorldRegistry, Move.Entity, InstanceRoot, Move.bPreserveWorld);
            }
        }
        for (const FInstanceMove& Move : Moves)
        {
            if (Move.Parent != InstanceRoot)
            {
                ECS::Utils::ReparentEntity(WorldRegistry, Move.Entity, Move.Parent, Move.bPreserveWorld);
            }
        }

        WorldRegistry.Get<SPrefabInstanceComponent>(InstanceRoot).AppliedRevision = Revision;

        // Newly added entities + reparented ones need their world matrices recomputed.
        MarkSubtreeTransformsDirty(WorldRegistry, InstanceRoot);
    }

    void CPrefab::RefreshStaleInstances(CWorld* World)
    {
        RefreshInstancesInWorld(World, /*bOnlyStale*/ true);
    }

    void CPrefab::RefreshAllInstancesInWorld(CWorld* World)
    {
        if (World != nullptr)
        {
            // InitializeWorld culls the pending set pre-swap, but other paths reach this without that step.
            CullOrphanedInstances(World->GetEntityRegistry());
        }
        RefreshInstancesInWorld(World, /*bOnlyStale*/ false);
    }

    void CPrefab::RefreshInstancesInWorld(CWorld* World, bool bOnlyStale)
    {
        if (World == nullptr)
        {
            return;
        }

        // Gathered first, since a refresh adds and destroys instance entities.
        ECS::FRegistry& WorldRegistry = World->GetEntityRegistry();
        TVector<ECS::FEntity> Roots;
        WorldRegistry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
        {
            const CPrefab* Source = Inst.SourcePrefab.Get();
            if (Inst.bIsRoot && Source != nullptr && (!bOnlyStale || Inst.AppliedRevision != Source->GetRevision()))
            {
                Roots.Add(E);
            }
        });

        for (ECS::FEntity Root : Roots)
        {
            if (const SPrefabInstanceComponent* Inst = WorldRegistry.TryGet<SPrefabInstanceComponent>(Root); Inst != nullptr && Inst->SourcePrefab != nullptr)
            {
                Inst->SourcePrefab->RefreshInstance(World, Root);
            }
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

            ECS::FRegistry& WorldRegistry = World->GetEntityRegistry();

            TVector<ECS::FEntity> Roots;
            WorldRegistry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
            {
                if (Inst.bIsRoot && Inst.SourcePrefab.Get() == this)
                {
                    Roots.Add(E);
                }
            });

            if (Roots.IsEmpty())
            {
                continue;
            }

            if (GLoadedWorldRefreshHook)
            {
                GLoadedWorldRefreshHook(World, true);
            }
            for (ECS::FEntity Root : Roots)
            {
                RefreshInstance(World, Root);
            }
            if (GLoadedWorldRefreshHook)
            {
                GLoadedWorldRefreshHook(World, false);
            }
        }
    }

    void CPrefab::SetLoadedWorldRefreshHook(TFunction<void(CWorld*, bool)> Hook)
    {
        GLoadedWorldRefreshHook = Move(Hook);
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
                Orphans.Add(E);
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

            ECS::FRegistry& WorldRegistry = World->GetEntityRegistry();

            // A detached subtree has no instance component, so it is not matched and survives.
            TVector<ECS::FEntity> Matching;
            WorldRegistry.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
            {
                if (Inst.SourcePrefab.Get() == this)
                {
                    Matching.Add(E);
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

        ECS::FRegistry& WorldRegistry = World->GetEntityRegistry();
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
        ToStrip.Reserve(16);
        ToStrip.Add(InstanceRoot);
        ForEachOwnInstanceNode(WorldRegistry, InstanceRoot, [&](ECS::FEntity Desc)
        {
            const SPrefabInstanceComponent* Inst = WorldRegistry.TryGet<SPrefabInstanceComponent>(Desc);
            if (Inst != nullptr && Inst->SourcePrefab.Get() == Source)
            {
                ToStrip.Add(Desc);
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

    void CPrefab::CaptureFromWorld(CWorld* SourceWorld, ECS::FEntity RootEntity, bool bAdoptSourceAsInstance)
    {
        if (SourceWorld == nullptr)
        {
            return;
        }

        ECS::FRegistry& WorldRegistry = SourceWorld->GetEntityRegistry();
        if (!WorldRegistry.IsValid(RootEntity))
        {
            return;
        }

        TVector<ECS::FEntity> EntitiesToCapture;
        EntitiesToCapture.Reserve(16);
        EntitiesToCapture.Add(RootEntity);
        WorldRegistry.GetHierarchy().ForEachDescendant(RootEntity, [&](ECS::FEntity E)
        {
            EntitiesToCapture.Add(E);
        });

        // Placed instances key their root by this, so an untagged source must not mint a new one for it.
        FName PreviousRootID;
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity PrefabE, const SPrefabComponent& PrefabComp)
        {
            if (Registry.GetHierarchy().GetParent(PrefabE) == ECS::NullEntity && PreviousRootID.IsNone())
            {
                PreviousRootID = PrefabComp.StableID;
            }
        });

        // CopyRegistry remaps hierarchy and handle fields, and skips nested instance tracking.
        MarkRegistryChanged();
        Registry = ECS::FRegistry{};
        THashMap<ECS::FEntity, ECS::FEntity> Map;
        CopyRegistry(WorldRegistry, Registry, Map, &EntitiesToCapture, &ShouldSkipInstanceComponent);

        // Only this prefab's own placed nodes keep their ids, since a nested placement's ids repeat once per copy.
        THashMap<ECS::FEntity, FName> PlacedIDs;
        if (const SPrefabInstanceComponent* RootInst = WorldRegistry.TryGet<SPrefabInstanceComponent>(RootEntity);
            RootInst != nullptr && RootInst->bIsRoot && RootInst->SourcePrefab.Get() == this)
        {
            PlacedIDs.TryAdd(RootEntity, RootInst->StableID);
            ForEachOwnInstanceNode(WorldRegistry, RootEntity, [&](ECS::FEntity E)
            {
                if (const SPrefabInstanceComponent* Inst = WorldRegistry.TryGet<SPrefabInstanceComponent>(E); Inst != nullptr && Inst->SourcePrefab.Get() == this)
                {
                    PlacedIDs.TryAdd(E, Inst->StableID);
                }
            });
        }

        // Reuses an existing instance tag when present, so RefreshInstance can match placed counterparts.
        THashSet<FName> UsedIDs;
        for (ECS::FEntity SrcE : EntitiesToCapture)
        {
            auto It = Map.Find(SrcE);
            if (It == nullptr)
            {
                continue;
            }

            FName StableID;
            if (auto Placed = PlacedIDs.Find(SrcE); Placed != nullptr)
            {
                StableID = (*Placed);
            }
            if (StableID.IsNone() && SrcE == RootEntity)
            {
                StableID = PreviousRootID;
            }
            if (StableID.IsNone() || !UsedIDs.TryAdd(StableID))
            {
                StableID = GenerateStableID();
                UsedIDs.Add(StableID);
            }
            Registry.EmplaceOrReplace<SPrefabComponent>(*It).StableID = StableID;

            // An untagged source node would read as missing to the next refresh, which spawns a duplicate beside it.
            if (bAdoptSourceAsInstance)
            {
                SPrefabInstanceComponent& Instance = WorldRegistry.GetOrEmplace<SPrefabInstanceComponent>(SrcE);
                Instance.SourcePrefab = this;
                Instance.StableID     = StableID;
                Instance.bIsRoot      = (SrcE == RootEntity);

                // The source now is the prefab, so leaves it had overridden are no longer divergent, nested placements included.
                WorldRegistry.Remove<SPrefabOverrideComponent>(SrcE);
            }
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
                    Out.FindOrAdd(Comp.StableID, E);
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
            const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(E);
            return Parent != ECS::NullEntity ? StableIDOf(Registry, Parent) : FName();
        }

        CStruct* StructOfStorage(const ECS::FSparseSet& Storage)
        {
            return FindComponentStructByTypeId(Storage.GetTypeInfo().TypeID);
        }

        // The delta records it separately by StableID, so it is never diffed as component data.
        bool IsStructuralStorage(uint32 ID)
        {
            return ID == ECS::GetComponentTypeID<SPrefabComponent>();
        }

        void RemapAllHandles(ECS::FRegistry& Registry, const THashMap<ECS::FEntity, ECS::FEntity>& Map)
        {
            Registry.ForEachEntity([&](ECS::FEntity E)
            {
                ECS::Utils::RemapEntityReferences(Registry, E, Map, /*bClearUnmapped*/ false);
            });
        }

        // A key mapped to null has no counterpart, and inverting it would turn every null handle into that key.
        THashMap<ECS::FEntity, ECS::FEntity> InvertEntityMap(const THashMap<ECS::FEntity, ECS::FEntity>& Map)
        {
            THashMap<ECS::FEntity, ECS::FEntity> Out;
            for (const auto& [Key, Value] : Map)
            {
                if (Value != ECS::NullEntity)
                {
                    Out[Value] = Key;
                }
            }
            return Out;
        }

        // CopyRegistry nulls a handle whose target sits outside the copied set, so the targets join it.
        void AppendReferencedEntities(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Entities)
        {
            THashSet<ECS::FEntity> Present;
            Present.Reserve(Entities.Num());
            for (ECS::FEntity E : Entities)
            {
                Present.Add(E);
            }

            // Only the entities the caller supplied are walked, so an anchor cannot drag in a further one.
            TVector<ECS::FEntity> Referenced;
            const int32 OriginalCount = Entities.Num();
            for (int32 i = 0; i < OriginalCount; ++i)
            {
                Referenced.Reset();
                ECS::Utils::CollectEntityReferences(Registry, Entities[i], Referenced);
                for (ECS::FEntity Target : Referenced)
                {
                    if (Registry.IsValid(Target) && Present.TryAdd(Target))
                    {
                        Entities.Add(Target);
                    }
                }
            }
        }
    }

    // Walks two steps for every one of a trailing cursor, so a cycle above this prefab that does not reach Candidate ends the walk.
    bool CPrefab::IsDescendantOf(const CPrefab* Candidate) const
    {
        const CPrefab* Slow = this;
        for (const CPrefab* Fast = this; Fast != nullptr;)
        {
            if (Fast == Candidate)
            {
                return true;
            }
            Fast = Fast->ParentPrefab.Get();
            if (Fast == nullptr)
            {
                return false;
            }
            if (Fast == Candidate)
            {
                return true;
            }
            Fast = Fast->ParentPrefab.Get();
            Slow = Slow->ParentPrefab.Get();
            if (Fast == Slow)
            {
                return false;
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
                Out.Add(Candidate);
            }
        }
        return Out;
    }

    void CPrefab::ClearVariantDelta()
    {
        VariantDelta = ECS::FRegistry{};
        VariantOverriddenProperties.Reset();
        VariantAddedComponents.Reset();
        VariantRemovedComponents.Reset();
        VariantStructuralNodes.Reset();
        VariantRemovedEntities.Reset();
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
            LOG_ERROR("Prefab '{}' is in a variant cycle; leaving it unresolved.", *GetName());
            bVariantResolveFailed = true;
            return;
        }

        CPrefab* Parent = ParentPrefab.Get();
        if (Parent == this || Parent->IsDescendantOf(this))
        {
            LOG_ERROR("Prefab '{}' would parent onto its own descendant; leaving it unresolved.", *GetName());
            bVariantResolveFailed = true;
            return;
        }

        VisitStack.Add(this);
        Parent->ResolveVariantGuarded(VisitStack);
        VisitStack.Pop();

        // Resolving onto a parent that is itself empty would read as the variant having deleted everything.
        if (Parent->IsUnresolvedVariant())
        {
            LOG_ERROR("Prefab '{}' cannot resolve because its parent '{}' did not; leaving it unresolved.",
                *GetName(), *Parent->GetName());
            bVariantResolveFailed = true;
            return;
        }

        bVariantResolveFailed = false;

        MarkRegistryChanged();
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
            auto It = Resolved.Find(DeadID);
            if (It == nullptr || !Registry.IsValid(*It))
            {
                continue;
            }

            const ECS::FEntity Dead = *It;
            auto IsDoomed = [&](ECS::FEntity E)
            {
                const FName ID = StableIDOf(Registry, E);
                return E == Dead || (!ID.IsNone() && Algo::Contains(VariantRemovedEntities, ID));
            };

            // Only a survivor whose own parent dies is lifted, so a survivor's children stay with it.
            TVector<ECS::FEntity> Survivors;
            Registry.GetHierarchy().ForEachDescendant(Dead, [&](ECS::FEntity Desc)
            {
                if (!IsDoomed(Desc) && IsDoomed(Registry.GetHierarchy().GetParent(Desc)))
                {
                    Survivors.Add(Desc);
                }
            });
            // A prefab registry is never propagated, so its world matrices are stale and only locals can be trusted.
            for (ECS::FEntity S : Survivors)
            {
                if (Registry.IsValid(S) && Registry.HasAll<STransformComponent>(S))
                {
                    ECS::Utils::ReparentEntity(Registry, S, ECS::NullEntity, false);
                }
            }

            ECS::Utils::DestroyEntityHierarchy(Registry, Dead);
            Resolved.Remove(DeadID);
        }

        // An entity with no counterpart is one the variant adds, and the rest carry only its divergences.
        THashMap<FName, THashSet<FName>> RemovedByNode;
        for (const SPrefabComponentRef& Ref : VariantRemovedComponents)
        {
            RemovedByNode[Ref.EntityStableID].Add(Ref.ComponentType);
        }

        THashMap<FName, THashMap<FName, THashSet<FName>>> OverridesByNode;
        for (const SPrefabPropertyOverride& Override : VariantOverriddenProperties)
        {
            OverridesByNode[Override.EntityStableID][Override.ComponentType].Add(Override.PropertyPath);
        }

        THashSet<FName> AddedNodes;
        for (const SPrefabVariantNode& Node : VariantStructuralNodes)
        {
            if (Node.bAdded)
            {
                AddedNodes.Add(Node.StableID);
            }
        }

        // Added entities first, so a later reparent can address them.
        THashMap<ECS::FEntity, ECS::FEntity> DeltaToResolved;
        VariantDelta.View<SPrefabComponent>().ForEach([&](ECS::FEntity DeltaE, const SPrefabComponent& Comp)
        {
            if (Comp.StableID.IsNone())
            {
                return;
            }

            auto It = Resolved.Find(Comp.StableID);
            if (It != nullptr)
            {
                DeltaToResolved[DeltaE] = (*It);
                return;
            }

            // The parent deleted a node this variant only overrode, so its overrides have nothing left to land on.
            if (!AddedNodes.Contains(Comp.StableID))
            {
                DeltaToResolved[DeltaE] = ECS::NullEntity;
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
            auto DestIt = DeltaToResolved.Find(DeltaE);
            if (DestIt == nullptr || !Registry.IsValid(*DestIt))
            {
                return;
            }

            const ECS::FEntity DestE = (*DestIt);
            const THashMap<FName, THashSet<FName>>* NodeOverrides = nullptr;
            if (auto OIt = OverridesByNode.Find(Comp.StableID); OIt != nullptr)
            {
                NodeOverrides = &(*OIt);
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
                    if (auto LIt = NodeOverrides->Find(CompStruct->GetName()); LIt != nullptr)
                    {
                        Leaves = &(*LIt);
                    }
                }

                // Otherwise write just the leaves the variant authors.
                if (DstPtr == nullptr || Leaves == nullptr || CompStruct == nullptr)
                {
                    Ops->EmplaceCopy(Registry, DestE, SrcPtr);
                }
                // A variant captured before scripts were diffed per field recorded the whole component, and resolves as it always did.
                else if (CompStruct == SEntityScriptComponent::StaticStruct() && !Leaves->Contains(PrefabOverride::WholeValuePath()))
                {
                    ApplyOverriddenScriptLeaves(*static_cast<SEntityScriptComponent*>(DstPtr), *static_cast<const SEntityScriptComponent*>(SrcPtr), *Leaves);
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
            auto It = Resolved.Find(NodeID);
            if (It == nullptr || !Registry.IsValid(*It))
            {
                continue;
            }

            TVector<uint32> ToRemove;
            for (Lumina::ECS::FSparseSet* StoragePtr : Registry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& Storage = *StoragePtr;
                if (IsStructuralStorage(ID) || !Storage.Contains(*It)) continue;

                CStruct* CompStruct = StructOfStorage(Storage);
                if (CompStruct != nullptr && CompNames.Contains(CompStruct->GetName()))
                {
                    ToRemove.Add(ID);
                }
            }

            for (uint32 ID : ToRemove)
            {
                if (auto* Storage = Registry.FindStorage(ID))
                {
                    Storage->RemoveEntity(*It);
                }
            }
        }

        // Parentage last, since every node the delta addresses now exists.
        TVector<TPair<ECS::FEntity, ECS::FEntity>> Moves;
        for (const SPrefabVariantNode& Node : VariantStructuralNodes)
        {
            auto ChildIt = Resolved.Find(Node.StableID);
            if (ChildIt == nullptr || !Registry.IsValid(*ChildIt))
            {
                continue;
            }

            // ReparentEntity reads the child's transform, and an added node may not have carried one.
            if (!Registry.HasAll<STransformComponent>(*ChildIt))
            {
                Registry.Emplace<STransformComponent>(*ChildIt);
            }

            ECS::FEntity NewParent = ECS::NullEntity;
            if (!Node.ParentStableID.IsNone())
            {
                auto ParentIt = Resolved.Find(Node.ParentStableID);
                if (ParentIt == nullptr || !Registry.IsValid(*ParentIt))
                {
                    continue;
                }
                NewParent = (*ParentIt);
            }
            Moves.Add({ (*ChildIt), NewParent });
        }

        // Every mover lifts out before any lands, since a swap applied one move at a time passes through a cycle.
        for (const auto& [Child, NewParent] : Moves)
        {
            ECS::Utils::ReparentEntity(Registry, Child, ECS::NullEntity, false);
        }
        for (const auto& [Child, NewParent] : Moves)
        {
            if (NewParent == ECS::NullEntity)
            {
                continue;
            }
            // The parent prefab can since have nested the new parent beneath this node, which no order resolves.
            if (Registry.GetHierarchy().IsDescendantOf(NewParent, Child))
            {
                LOG_WARN("Prefab '{}' moves '{}' under '{}', which its parent prefab now nests beneath it; leaving it at the root.",
                    *GetName(), *StableIDOf(Registry, Child), *StableIDOf(Registry, NewParent));
                continue;
            }
            ECS::Utils::ReparentEntity(Registry, Child, NewParent, false);
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
                      "overwriting it with a diff of empty data.", *GetName());
            return;
        }

        ClearVariantDelta();
        BumpDataGeneration();

        THashMap<FName, ECS::FEntity> MineByID = IndexByStableID(Registry);

        // The two registries number entities independently, so the parent is diffed through a copy whose handles name this registry's ids.
        ECS::FRegistry ParentView;
        {
            THashMap<ECS::FEntity, ECS::FEntity> ParentToView;
            CopyRegistry(Parent->Registry, ParentView, ParentToView);

            THashMap<ECS::FEntity, ECS::FEntity> ViewToMine;
            ParentView.View<SPrefabComponent>().ForEach([&](ECS::FEntity ViewE, const SPrefabComponent& Comp)
            {
                auto It = MineByID.Find(Comp.StableID);
                ViewToMine[ViewE] = It != nullptr ? (*It) : ECS::NullEntity;
            });
            ParentView.ForEachEntity([&](ECS::FEntity ViewE)
            {
                ECS::Utils::RemapEntityReferences(ParentView, ViewE, ViewToMine, /*bClearUnmapped*/ true);
            });
        }

        THashMap<FName, ECS::FEntity> ParentByID = IndexByStableID(ParentView);

        // Entities the parent still has and this variant dropped.
        for (auto& [StableID, ParentE] : ParentByID)
        {
            if (!MineByID.Contains(StableID))
            {
                VariantRemovedEntities.Add(StableID);
            }
        }

        // Collected first so the delta registry is built in one copy.
        TVector<ECS::FEntity> DivergedEntities;
        THashMap<FName, THashSet<FName>> KeepComponentsByNode;

        for (auto& [StableID, MineE] : MineByID)
        {
            auto ParentIt = ParentByID.Find(StableID);
            const bool bAdded = ParentIt == nullptr;

            if (bAdded)
            {
                DivergedEntities.Add(MineE);
                VariantStructuralNodes.Emplace(StableID, ParentStableIDOf(Registry, MineE), true);
                continue;
            }

            const ECS::FEntity ParentE = (*ParentIt);

            if (ParentStableIDOf(Registry, MineE) != ParentStableIDOf(ParentView, ParentE))
            {
                VariantStructuralNodes.Emplace(StableID, ParentStableIDOf(Registry, MineE), false);
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

                auto* ParentStorage = ParentView.FindStorage(ID);
                const bool bParentHas = ParentStorage != nullptr && ParentStorage->Contains(ParentE);

                if (!bParentHas)
                {
                    VariantAddedComponents.Emplace(StableID, CompStruct->GetName());
                    KeepComponentsByNode[StableID].Add(CompStruct->GetName());
                    bNodeDiverged = true;
                    continue;
                }

                TVector<FName> Leaves;
                if (CompStruct == SEntityScriptComponent::StaticStruct())
                {
                    CollectScriptOverrides(*static_cast<const SEntityScriptComponent*>(MyStorage.GetRaw(MineE)),
                        *static_cast<const SEntityScriptComponent*>(ParentStorage->GetRaw(ParentE)), Leaves);
                }
                else
                {
                    PrefabOverride::CollectOverriddenLeaves(CompStruct, MyStorage.GetRaw(MineE),
                        ParentStorage->GetRaw(ParentE), Leaves);
                }

                for (const FName& Path : Leaves)
                {
                    VariantOverriddenProperties.Emplace(StableID, CompStruct->GetName(), Path);
                }

                if (!Leaves.IsEmpty())
                {
                    KeepComponentsByNode[StableID].Add(CompStruct->GetName());
                    bNodeDiverged = true;
                }
            }

            // Components the parent ships and this variant deleted.
            for (Lumina::ECS::FSparseSet* ParentStoragePtr : ParentView.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = ParentStoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& ParentStorage = *ParentStoragePtr;
                if (IsStructuralStorage(ID) || IsNonReplicatedStorage(ID)) continue;
                if (!ParentStorage.Contains(ParentE)) continue;

                auto* MyStorage = Registry.FindStorage(ID);
                if (MyStorage != nullptr && MyStorage->Contains(MineE)) continue;

                if (CStruct* CompStruct = StructOfStorage(ParentStorage))
                {
                    VariantRemovedComponents.Emplace(StableID, CompStruct->GetName());
                }
            }

            if (bNodeDiverged)
            {
                DivergedEntities.Add(MineE);
            }
        }

        if (DivergedEntities.IsEmpty())
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
            auto KeepIt = KeepComponentsByNode.Find(Comp.StableID);
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
                    && KeepIt != nullptr
                    && KeepIt->Contains(CompStruct->GetName());

                if (!bKeep)
                {
                    ToStrip.Add(ID);
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

        while (!Frontier.IsEmpty())
        {
            TVector<CPrefab*> Next;
            for (CPrefab* Variant : Frontier)
            {
                if (!Seen.TryAdd(Variant))
                {
                    continue;
                }

                // The refresh moves an instance child only while it still sits at the old value, so the old values are kept.
                Variant->CapturePreCommitState();
                Variant->ResolveVariant();
                Variant->RefreshInstancesInLoadedWorlds();
                Variant->ClearPreCommitState();

                for (CPrefab* Child : Variant->FindDirectVariants())
                {
                    Next.Add(Child);
                }
            }
            Frontier = Move(Next);
        }
    }

    void CPrefab::RebuildStableIDLookup()
    {
        StableIDLookup.Reset();
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity E, const SPrefabComponent& Comp)
        {
            if (!Comp.StableID.IsNone())
            {
                // A duplicate id is a bug elsewhere, and picking the same entity keeps the baseline stable.
                StableIDLookup.FindOrAdd(Comp.StableID, E);
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

        auto It = StableIDLookup.Find(StableID);
        if (It == nullptr)
        {
            return ECS::NullEntity;
        }

        // A missed generation bump would hand back a recycled entity, and a wrong component is worse than none.
        const ECS::FEntity Cached = (*It);
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
            Cur = Registry.GetHierarchy().GetParent(Cur);
        }
        return ECS::NullEntity;
    }

    namespace
    {
        // One instance node's component, its prefab baseline and the root whose ledger records it.
        struct FOverrideTarget
        {
            ECS::FEntity Root = ECS::NullEntity;
            CPrefab*     Source = nullptr;
            CStruct*     ComponentType = nullptr;
            FName        NodeID;
            void*        InstancePtr = nullptr;
            void*        PrefabPtr = nullptr;
        };

        bool ResolveOverrideTarget(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType, FOverrideTarget& Out)
        {
            if (ComponentType == nullptr || IsInstanceTrackingStruct(ComponentType) || !Registry.IsValid(Entity))
            {
                return false;
            }

            // A script field edit names the script's class, but the override belongs to the component holding the script.
            Out.ComponentType = AsScriptClass(ComponentType) != nullptr ? SEntityScriptComponent::StaticStruct() : ComponentType;

            const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Entity);
            Out.Source = Inst != nullptr ? Inst->SourcePrefab.Get() : nullptr;
            Out.Root = Out.Source != nullptr ? CPrefab::FindInstanceRoot(Registry, Entity) : ECS::NullEntity;
            if (Out.Root == ECS::NullEntity)
            {
                return false;
            }

            Out.NodeID = Inst->StableID;
            Out.InstancePtr = FindReflectedComponentPtr(Registry, Entity, Out.ComponentType);

            // Null when the component is instance-added, which has no per-leaf tracking.
            Out.PrefabPtr = Out.Source->ResolvePrefabComponentPtr(Out.NodeID, Out.ComponentType);
            return true;
        }

        void SetLedgerPaths(ECS::FRegistry& Registry, const FOverrideTarget& Target, const TVector<FName>& Paths)
        {
            SPrefabOverrideComponent& Ledger = Registry.GetOrEmplace<SPrefabOverrideComponent>(Target.Root);
            const FName CompName = Target.ComponentType->GetName();
            auto& Recs = Ledger.PropertyOverrides;
            Recs.RemoveAll([&](const SPrefabPropertyOverride& O)
            {
                return O.EntityStableID == Target.NodeID && O.ComponentType == CompName;
            });

            for (const FName& Path : Paths)
            {
                Recs.Emplace(Target.NodeID, CompName, Path);
            }
        }
    }

    void CPrefab::RecaptureComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType)
    {
        FOverrideTarget Target;
        if (!ResolveOverrideTarget(Registry, Entity, ComponentType, Target))
        {
            return;
        }

        TVector<FName> NewPaths;
        if (Target.InstancePtr != nullptr && Target.PrefabPtr != nullptr)
        {
            // Handles are compared in the instance's ids, or an inherited one reads as edited and a world one can pass for inherited.
            const FRemappedPrefabValue Baseline(Target.ComponentType, Target.PrefabPtr, MapPrefabOntoInstance(*Target.Source, Registry, Target.Root));
            if (Target.ComponentType == SEntityScriptComponent::StaticStruct())
            {
                CollectScriptOverrides(*static_cast<const SEntityScriptComponent*>(Target.InstancePtr), *static_cast<const SEntityScriptComponent*>(Baseline.Get()), NewPaths);
            }
            else
            {
                PrefabOverride::CollectOverriddenLeaves(Target.ComponentType, Target.InstancePtr, Baseline.Get(), NewPaths);
            }
        }

        SetLedgerPaths(Registry, Target, NewPaths);
    }

    TVector<FName> CPrefab::GetComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType)
    {
        TVector<FName> Paths;
        FOverrideTarget Target;
        if (ResolveOverrideTarget(Registry, Entity, ComponentType, Target))
        {
            if (const SPrefabOverrideComponent* Ledger = Registry.TryGet<SPrefabOverrideComponent>(Target.Root))
            {
                for (const SPrefabPropertyOverride& Override : Ledger->PropertyOverrides)
                {
                    if (Override.EntityStableID == Target.NodeID && Override.ComponentType == Target.ComponentType->GetName())
                    {
                        Paths.Add(Override.PropertyPath);
                    }
                }
            }
        }
        return Paths;
    }

    void CPrefab::RestoreComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType, const TVector<FName>& Paths)
    {
        FOverrideTarget Target;
        if (!ResolveOverrideTarget(Registry, Entity, ComponentType, Target))
        {
            return;
        }

        SetLedgerPaths(Registry, Target, Paths);
        if (Target.InstancePtr == nullptr || Target.PrefabPtr == nullptr)
        {
            return;
        }

        // The restored value may predate a prefab edit, so every leaf it did not override takes the prefab's current one.
        THashSet<FName> Overridden;
        Overridden.Append(Paths);
        const FRemappedPrefabValue Inherited(Target.ComponentType, Target.PrefabPtr, MapPrefabOntoInstance(*Target.Source, Registry, Target.Root));
        if (Target.ComponentType == SEntityScriptComponent::StaticStruct())
        {
            ApplyInheritedScriptLeaves(Registry, Entity, *static_cast<const SEntityScriptComponent*>(Inherited.Get()), Overridden, nullptr);
        }
        else
        {
            PrefabOverride::ApplyInheritedLeaves(Target.ComponentType, Target.InstancePtr, Inherited.Get(), Overridden);
        }
    }

    void CPrefab::MarkInstanceStale(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const ECS::FEntity Root = FindInstanceRoot(Registry, Entity);
        if (Root != ECS::NullEntity)
        {
            Registry.Get<SPrefabInstanceComponent>(Root).AppliedRevision = kStaleRevision;
        }
    }

    void CPrefab::NoteComponentAdded(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || IsInstanceTrackingStruct(ComponentType) || !Registry.IsValid(Entity))
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
        Removed.RemoveAll(MatchesPair);

        if (!bPrefabHas)
        {
            auto& Added = Ledger.AddedComponents;
            if (!Algo::AnyOf(Added, MatchesPair))
            {
                SPrefabComponentRef Rec;
                Rec.EntityStableID = NodeID;
                Rec.ComponentType  = CompName;
                Added.Add(Rec);
            }
        }
    }

    void CPrefab::NoteComponentRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType)
    {
        if (ComponentType == nullptr || IsInstanceTrackingStruct(ComponentType) || !Registry.IsValid(Entity))
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
        Props.RemoveAll(MatchesPair);

        auto& Added = Ledger.AddedComponents;
        Added.RemoveAll(MatchesPair);

        auto& Removed = Ledger.RemovedComponents;
        Removed.RemoveAll(MatchesPair);

        // An inherited component the user deleted must be recorded so refresh won't re-add it.
        if (bPrefabHas)
        {
            SPrefabComponentRef Rec;
            Rec.EntityStableID = NodeID;
            Rec.ComponentType  = CompName;
            Removed.Add(Rec);
        }
    }

    namespace
    {
        template<typename FuncType>
        void ForEachRemovedInheritedNode(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Doomed, FuncType&& Func)
        {
            for (ECS::FEntity Entity : Doomed)
            {
                if (!Registry.IsValid(Entity))
                {
                    continue;
                }
                const SPrefabInstanceComponent* Inst = Registry.TryGet<SPrefabInstanceComponent>(Entity);
                if (Inst == nullptr || Inst->bIsRoot || Inst->StableID.IsNone())
                {
                    continue;
                }

                // A root going down with its nodes leaves no instance to keep a record on.
                const ECS::FEntity Root = CPrefab::FindInstanceRoot(Registry, Entity);
                if (Root == ECS::NullEntity || Algo::Contains(Doomed, Root))
                {
                    continue;
                }
                const FName NodeID = Inst->StableID;
                Func(Root, NodeID);
            }
        }
    }

    void CPrefab::CollectRemovedNodeRoots(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Doomed, TVector<ECS::FEntity>& OutRoots)
    {
        ForEachRemovedInheritedNode(Registry, Doomed, [&](ECS::FEntity Root, const FName&)
        {
            if (!Algo::Contains(OutRoots, Root))
            {
                OutRoots.Add(Root);
            }
        });
    }

    void CPrefab::NoteEntitiesRemoved(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Doomed)
    {
        ForEachRemovedInheritedNode(Registry, Doomed, [&](ECS::FEntity Root, const FName& NodeID)
        {
            TVector<FName>& Removed = Registry.GetOrEmplace<SPrefabOverrideComponent>(Root).RemovedEntities;
            if (!Algo::Contains(Removed, NodeID))
            {
                Removed.Add(NodeID);
            }
        });
    }
}
