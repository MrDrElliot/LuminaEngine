#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "EntityUtils.h"
#include "Components/CharacterComponent.h"
#include "Components/DirtyComponent.h"
#include "Components/EditorComponent.h"
#include "Components/EntityTags.h"
#include "Components/PhysicsComponent.h"
#include "Components/NameComponent.h"
#include "Components/RelationshipComponent.h"
#include "Networking/INetworkRuntime.h"
#include "Components/TagComponent.h"
#include "Components/TransformComponent.h"
#include "Systems/SystemAccess.h"
#include "Systems/SystemResources.h"
#include "Core/Assertions/Assert.h"
#include "Containers/ConcurrentQueue.h"
#include "TaskSystem/FiberSync.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Components/Component.h"
#include "Memory/SmartPtr.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include <atomic>
#include "Log/Log.h"

namespace Lumina
{
    // A reflected component reports its reflected name; anything else reports the spelling it registered under.
    FName GetAccessTypeName(uint32 Id)
    {
        const ECS::FComponentTypeRegistry& Types = ECS::FComponentTypeRegistry::Get();
        if (Id >= Types.Num())
        {
            return NAME_None;
        }

        const ECS::FComponentTypeInfo& Info = Types.GetInfo(static_cast<ECS::FComponentTypeID>(Id));
        if (CStruct* Struct = Info.GetBoundStruct())
        {
            return Struct->GetName();
        }
        return Info.Name;
    }

    void RegisterComponentOps(CStruct* Struct, const FComponentOps* Ops)
    {
        if (Struct == nullptr || Ops == nullptr)
        {
            return;
        }

        Struct->SetComponentOps(Ops);
        ECS::FComponentTypeRegistry::Get().BindStruct(static_cast<ECS::FComponentTypeID>(Ops->TypeId), Struct);
    }

    CStruct* FindComponentStruct(FStringView Name)
    {
        CStruct* Struct = FindObject<CStruct>(FName(Name));
        return (Struct != nullptr && Struct->GetComponentOps() != nullptr) ? Struct : nullptr;
    }

    CStruct* FindComponentStructByTypeId(uint64 TypeId)
    {
        const ECS::FComponentTypeRegistry& Types = ECS::FComponentTypeRegistry::Get();
        if (TypeId >= Types.Num())
        {
            return nullptr;
        }
        return Types.GetInfo(static_cast<ECS::FComponentTypeID>(TypeId)).GetBoundStruct();
    }

    const FComponentOps* FindComponentOps(FStringView Name)
    {
        CStruct* Struct = FindComponentStruct(Name);
        return Struct != nullptr ? Struct->GetComponentOps() : nullptr;
    }

    void ForEachComponentStruct(const TFunction<void(CStruct*)>& Function)
    {
        const ECS::FComponentTypeRegistry& Types = ECS::FComponentTypeRegistry::Get();
        for (size_t TypeId = 0; TypeId < Types.Num(); ++TypeId)
        {
            // Only a type that went through RegisterComponentOps is a component.
            if (CStruct* Struct = Types.GetInfo(static_cast<ECS::FComponentTypeID>(TypeId)).GetBoundStruct())
            {
                Function(Struct);
            }
        }
    }

    //~ Honest-access validation, one thread_local per Runtime thread; see SystemAccess.h.
#if !defined(LE_SHIPPING)
    namespace
    {
        thread_local const FSystemAccess* GExecutingSystemAccess = nullptr;
    }

    void SetExecutingSystemAccess(const FSystemAccess* Access)
    {
        GExecutingSystemAccess = Access;
    }

    const FSystemAccess* GetExecutingSystemAccess()
    {
        return GExecutingSystemAccess;
    }

    void ValidateSystemAccess(uint32 ComponentId, bool bWrite, const char* What)
    {
        const FSystemAccess* Access = GExecutingSystemAccess;
        if (Access == nullptr)
        {
            return; // not inside a scheduled system Update (gameplay/editor/tool call) -> nothing to check
        }

        const bool bDeclared = bWrite ? Access->DeclaresWrite(ComponentId) : Access->DeclaresRead(ComponentId);
        if (bDeclared)
        {
            return;
        }

        // Log each unique missing access once so a per-entity loop cannot spam, then assert in debug.
        static FFiberMutex SeenMutex;
        static THashSet<uint64> Seen;
        const uint64 Key = (reinterpret_cast<uint64>(Access) ^ (uint64(ComponentId) << 1)) ^ (bWrite ? 1ull : 0ull);
        bool bFirst = false;
        {
            FFiberScopeLock Lock(SeenMutex);
            bFirst = Seen.insert(Key).second;
        }
        if (bFirst)
        {
            const FName TypeName = GetAccessTypeName(ComponentId);
            LOG_ERROR("System ran in parallel but under-declared its ECS access: it touched '{}' ({}), which it did "
                      "not declare. Add it to the system's FSystemAccess (or drop the Access member to run "
                      "exclusive). This is a silent data race under concurrent scheduling.",
                      TypeName.IsNone() ? FName("<unregistered type>") : TypeName, What);
            DEBUG_ASSERT(false, "System under-declared ECS access (see log).");
        }
    }

    namespace
    {
        // Set on a registry once its hooks are installed, so a re-init cannot stack duplicate listeners.
        struct FAccessValidatorsConnected {};

        TVector<void (*)(ECS::FRegistry&)>& ComponentAccessValidators()
        {
            static TVector<void (*)(ECS::FRegistry&)> Connectors;
            return Connectors;
        }

        void ValidateEntityStructuralWrite(ECS::FRegistry&, ECS::FEntity)
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<SystemResource::EntityStructure>()),
                true, "Write<SystemResource::EntityStructure>");
        }
    }

    void RegisterComponentAccessValidator(void (*Connect)(ECS::FRegistry&))
    {
        ComponentAccessValidators().push_back(Connect);
    }

    void ConnectComponentAccessValidators(ECS::FRegistry& Registry)
    {
        if (Registry.Ctx().Contains<FAccessValidatorsConnected>())
        {
            return;
        }
        Registry.Ctx().Emplace<FAccessValidatorsConnected>();

        for (void (*Connect)(ECS::FRegistry&) : ComponentAccessValidators())
        {
            Connect(Registry);
        }

        // Create and destroy both route through the registry's entity records, so this covers every path.
        Registry.OnEntityCreated().Connect<&ValidateEntityStructuralWrite>();
        Registry.OnEntityDestroyed().Connect<&ValidateEntityStructuralWrite>();
    }
#endif
}

namespace Lumina::ECS::Utils
{
    struct FComponentTypeCache
    {
        TVector<uint32> SiblingIndices;

        struct FEntry
        {
            ECS::FSparseSet*   Set;
            CStruct*            Struct;
        };
        TVector<FEntry> Entries;

        void Build(ECS::FRegistry& Registry)
        {
            Entries.clear();
            for (Lumina::ECS::FSparseSet* SetPtr : Registry.GetActiveStorages())
            {
                ECS::FSparseSet& Set = *SetPtr;
                if (Set.IsEmpty())
                {
                    continue;
                }

                if (CStruct* StructType = FindComponentStructByTypeId(Set.GetTypeInfo().TypeID))
                {
                    Entries.push_back({ &Set, StructType });
                }
            }
        }
    };

    // A null cache builds one for this entity alone, which is pointless for a bulk save.
    static bool SerializeEntityWrite(FArchive& RESTRICT Ar, ECS::FRegistry& RESTRICT Registry,
                                     ECS::FEntity& RESTRICT Entity, const FComponentTypeCache* Cache);

    LUM_DISABLE_DEPRECATION_WARNINGS
    // Goes with REGISTRY_PARENT_LINKS's predecessor format, so it is deleted together with FRelationshipComponent.
    static void ReadLegacyRelationship(FArchive& Ar, ECS::FEntity Entity, FParentLinks* Links)
    {
        bool bHasRelationship = false;
        Ar << bHasRelationship;
        if (!bHasRelationship)
        {
            return;
        }

        FRelationshipComponent Legacy;
        Ar << Legacy.Children << Legacy.First << Legacy.Prev << Legacy.Next << Legacy.Parent;
        if (Links == nullptr || Legacy.Parent.IsNull())
        {
            return;
        }

        Links->Links.push_back({ Entity, Legacy.Parent, ECS::FHierarchy::AppendPosition });
        if (!Legacy.Next.IsNull())
        {
            Links->LegacyNextSibling[Entity] = Legacy.Next;
        }
    }
    LUM_RESTORE_DEPRECATION_WARNINGS

    void FParentLinks::Apply(ECS::FRegistry& Registry)
    {
        // A legacy file only names each child's next sibling, so its indices come from walking those chains.
        if (!LegacyNextSibling.empty())
        {
            THashMap<ECS::FEntity, FLink*> ByChild;
            THashSet<ECS::FEntity> HasPrev;
            for (FLink& Link : Links)
            {
                ByChild[Link.Child] = &Link;
            }
            for (const auto& [Child, Next] : LegacyNextSibling)
            {
                HasPrev.insert(Next);
            }
            for (FLink& Head : Links)
            {
                if (HasPrev.contains(Head.Child))
                {
                    continue;
                }
                uint32 Index = 0;
                for (FLink* Cursor = &Head; Cursor != nullptr; ++Index)
                {
                    Cursor->SiblingIndex = Index;
                    const auto Next = LegacyNextSibling.find(Cursor->Child);
                    const auto NextLink = Next != LegacyNextSibling.end() ? ByChild.find(Next->second) : ByChild.end();
                    Cursor = NextLink != ByChild.end() && NextLink->second->Parent == Head.Parent ? NextLink->second : nullptr;
                }
            }
            LegacyNextSibling.clear();
        }

        // Ascending index per parent, so inserting each one at its index rebuilds the saved order.
        std::stable_sort(Links.begin(), Links.end(), [](const FLink& A, const FLink& B)
        {
            return A.Parent != B.Parent ? A.Parent < B.Parent : A.SiblingIndex < B.SiblingIndex;
        });

        for (const FLink& Link : Links)
        {
            if (Registry.IsValid(Link.Child) && Registry.IsValid(Link.Parent))
            {
                Registry.AttachChild(Link.Child, Link.Parent, Link.SiblingIndex);
            }
        }
        Links.clear();
    }

    bool SerializeEntity(FArchive& RESTRICT Ar, ECS::FRegistry& RESTRICT Registry, ECS::FEntity& RESTRICT Entity, FParentLinks* Links)
    {
        if (Ar.IsWriting())
        {
            return SerializeEntityWrite(Ar, Registry, Entity, nullptr);
        }
        else if (Ar.IsReading())
        {
            Ar << Entity;

            if (!Registry.IsValid(Entity))
            {
                ECS::FEntity New = Registry.Create(Entity);
                ALERT_IF_NOT(New == Entity);
                Entity = New;
            }

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::REGISTRY_PARENT_LINKS)
            {
                ECS::FEntity Parent;
                uint32 SiblingIndex = 0;
                Ar << Parent << SiblingIndex;
                if (!Parent.IsNull())
                {
                    if (Links != nullptr)
                    {
                        Links->Links.push_back({ Entity, Parent, SiblingIndex });
                    }
                    else if (Registry.IsValid(Parent))
                    {
                        Registry.AttachChild(Entity, Parent, SiblingIndex);
                    }
                }
            }
            else
            {
                ReadLegacyRelationship(Ar, Entity, Links);
            }

            size_t NumComponents = 0;
            Ar << NumComponents;

            if (NumComponents > Ar.GetMaxSerializeSize())
            {
                LOG_ERROR("Archiver corrupted: entity claims {} components (max {})", NumComponents, Ar.GetMaxSerializeSize());
                Ar.SetHasError(true);
                return false;
            }

            for (size_t i = 0; i < NumComponents; ++i)
            {
                FName TypeName;
                Ar << TypeName;

                int64 ComponentSize = 0;
                Ar << ComponentSize;

                int64 ComponentStart = Ar.Tell();

                if (CStruct* Struct = FindObject<CStruct>(TypeName))
                {
                    LUMINA_PROFILE_SECTION_NAMED(Struct->GetName().c_str());
                    if (Struct == STagComponent::StaticStruct())
                    {
                        STagComponent NewTagComponent;
                        Struct->SerializeTaggedProperties(Ar, &NewTagComponent);
                        const ECS::TComponentStorage<STagComponent> TagStorage =
                            Registry.NamedStorage<STagComponent>(NewTagComponent.Tag);

                        if (!TagStorage.Contains(Entity))
                        {
                            TagStorage.Emplace(Entity, NewTagComponent);
                        }
                    }
                    else
                    {
                        if (const FComponentOps* Ops = Struct->GetComponentOps())
                        {
                            Ops->EmplaceSerialized(Registry, Entity, Ar);
                        }
                        else
                        {
                            LOG_WARN("[ECS] Entity {}: '{}' is reflected but not a component; skipping ({} bytes).",
                                (uint32)Entity, TypeName, ComponentSize);
                        }
                    }
                }
                else
                {
                    LOG_WARN("[ECS] Entity {}: skipping unknown component '{}' ({} bytes). Disabled plugin? Save will drop this component.", (uint32)Entity, TypeName, ComponentSize);
                }

                int64 ComponentEnd = ComponentSize + ComponentStart;
                Ar.Seek(ComponentEnd);
            }
        }
        
        return !Ar.HasError();
    }

    static bool SerializeEntityWrite(FArchive& RESTRICT Ar, ECS::FRegistry& RESTRICT Registry,
                                     ECS::FEntity& RESTRICT Entity, const FComponentTypeCache* Cache)
    {
        Ar << Entity;

        ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity);
        uint32 SiblingIndex = 0;
        if (!Parent.IsNull())
        {
            SiblingIndex = Cache != nullptr ? Cache->SiblingIndices[Entity.GetIndex()] : Registry.GetHierarchy().GetSiblingIndex(Entity);
        }
        Ar << Parent << SiblingIndex;

        int64 NumComponentsPos = Ar.Tell();
        size_t NumComponents = 0;
        Ar << NumComponents;

        // A one-entity write builds its own; the bulk path shares one across the whole registry.
        FComponentTypeCache LocalCache;
        if (Cache == nullptr)
        {
            LocalCache.Build(Registry);
            Cache = &LocalCache;
        }

        for (const FComponentTypeCache::FEntry& Entry : Cache->Entries)
        {
            if (!Entry.Set->Contains(Entity))
            {
                continue;
            }

            FName Name = Entry.Struct->GetName();
            Ar << Name;

            int64 ComponentStart = Ar.Tell();

            int64 ComponentSize = 0;
            Ar << ComponentSize;

            int64 StartOfComponentData = Ar.Tell();

            {
                LUMINA_PROFILE_SECTION_NAMED(Name.c_str());
                Entry.Struct->SerializeTaggedProperties(Ar, Entry.Set->GetRaw(Entity));
            }

            int64 EndOfComponentData = Ar.Tell();

            ComponentSize = EndOfComponentData - StartOfComponentData;

            Ar.Seek(ComponentStart);
            Ar << ComponentSize;
            Ar.Seek(EndOfComponentData);

            NumComponents++;
        }

        int64 SizeBefore = Ar.Tell();
        Ar.Seek(NumComponentsPos);
        Ar << NumComponents;
        Ar.Seek(SizeBefore);

        return !Ar.HasError();
    }

    bool SerializeRegistry(FArchive& Ar, ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();
        if (Ar.IsWriting())
        {
            Registry.Compact();

            // Built once for the whole walk, which is what FComponentTypeCache saves per entity.
            FComponentTypeCache TypeCache;
            TypeCache.Build(Registry);
            Registry.GetHierarchy().BuildSiblingIndices(TypeCache.SiblingIndices);

            int64 PreSerializePos = Ar.Tell();

            int32 NumEntitiesSerialized = 0;
            Ar << NumEntitiesSerialized;

            Registry.ForEachEntityExcept<FEditorComponent>([&](ECS::FEntity Entity)
            {
                int64 PreEntityPos = Ar.Tell();

                int64 EntitySaveSize = 0;
                Ar << EntitySaveSize;

                bool bSuccess = SerializeEntityWrite(Ar, Registry, Entity, &TypeCache);
                if (!bSuccess)
                {
                    // Rewind to before this entity's data and continue with next entity
                    Ar.Seek(PreEntityPos);
                    return;
                }

                NumEntitiesSerialized++;

                int64 PostEntityPos = Ar.Tell();

                // Calculate actual size written (excluding the size field itself)
                EntitySaveSize = PostEntityPos - PreEntityPos - sizeof(int64);
        
                // Go back and write the correct size
                Ar.Seek(PreEntityPos);
                Ar << EntitySaveSize;
        
                // Return to end position to continue with next entity
                Ar.Seek(PostEntityPos);
            });
    
            int64 PostSerializePos = Ar.Tell();

            LOG_INFO("[ECS] Saved registry: {} entities written, {} live in the registry",
                NumEntitiesSerialized, Registry.NumEntities());

            // Go back and write the actual number of successfully serialized entities
            Ar.Seek(PreSerializePos);
            Ar << NumEntitiesSerialized;

            // Return to end of all serialized data
            Ar.Seek(PostSerializePos);
        }
        else if (Ar.IsReading())
        {
            int32 NumEntitiesSerialized = 0;
            Ar << NumEntitiesSerialized;

            if (NumEntitiesSerialized < 0 || (size_t)NumEntitiesSerialized > Ar.GetMaxSerializeSize())
            {
                LOG_ERROR("Archiver corrupted: registry claims {} entities (max {})", NumEntitiesSerialized, Ar.GetMaxSerializeSize());
                Ar.SetHasError(true);
                return false;
            }

            LOG_INFO("[ECS] Loading registry: {} entities claimed by the archive", NumEntitiesSerialized);

            // A child can come before its parent in the file, so links attach once every entity exists.
            FParentLinks Links;

            for (int32 i = 0; i < NumEntitiesSerialized; ++i)
            {
                int64 EntitySaveSize = 0;
                Ar << EntitySaveSize;

                int64 PreEntityPos = Ar.Tell();

                ECS::FEntity NewEntity = ECS::NullEntity;
                bool bSuccess = ECS::Utils::SerializeEntity(Ar, Registry, NewEntity, &Links);

                // Clear the per-entity error so one corrupt entity cannot poison the calls after it.
                Ar.SetHasError(false);

                if (!bSuccess || NewEntity == ECS::NullEntity)
                {
                    // Skip to the next entity using the saved size
                    LOG_ERROR("Failed to serialize entity: {}", NewEntity.Value);
                    Ar.Seek(PreEntityPos + EntitySaveSize);
                    continue;
                }

                Registry.EmplaceOrReplace<FNeedsTransformUpdate>(NewEntity);

                int64 PostEntityPos = Ar.Tell();
                int64 ActualBytesRead = PostEntityPos - PreEntityPos;

                if (ActualBytesRead != EntitySaveSize)
                {
                    // Data mismatch, seek to correct position to stay aligned
                    LOG_ERROR("Entity Serialization Mismatch For {}: Expected: {} - Read: {}", NewEntity.Value, EntitySaveSize, ActualBytesRead);
                    Ar.Seek(PreEntityPos + EntitySaveSize);
                }
            }

            Links.Apply(Registry);
        }

        return !Ar.HasError();
    }

    bool EntityHasTag(const FName& Tag, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return Registry.NamedStorage<STagComponent>(Tag).Contains(Entity);
    }

    void SetEntityTag(ECS::FRegistry& Registry, ECS::FEntity Entity, const FName& Tag)
    {
        if (!Registry.IsValid(Entity))
        {
            return;
        }

        // Out of the old storage first, or the entity answers to both names.
        ClearEntityTag(Registry, Entity);

        if (Tag.IsNone())
        {
            return;
        }

        STagComponent Component;
        Component.Tag = Tag;
        Registry.NamedStorage<STagComponent>(Tag).EmplaceOrReplace(Entity, Component);

        // A lookup by type alone cannot see into a named storage, so the unnamed copy records which one.
        Registry.GetOrEmplace<STagComponent>(Entity).Tag = Tag;
    }

    void ClearEntityTag(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (!Registry.IsValid(Entity))
        {
            return;
        }

        const STagComponent* Existing = Registry.TryGet<STagComponent>(Entity);
        if (Existing == nullptr)
        {
            return;
        }

        Registry.NamedStorage<STagComponent>(Existing->Tag).RemoveEntity(Entity);
        Registry.Remove<STagComponent>(Entity);
    }

    void RebuildTagStorages(ECS::FRegistry& Registry)
    {
        Registry.View<STagComponent>().ForEach([&](ECS::FEntity Entity, STagComponent& Component)
        {
            if (!Component.Tag.IsNone())
            {
                Registry.NamedStorage<STagComponent>(Component.Tag).EmplaceOrReplace(Entity, Component);
            }
        });
    }
    
    // --- Hierarchy ---

    void ReparentEntity(ECS::FRegistry& Registry, ECS::FEntity Child, ECS::FEntity Parent, bool bPreserveWorld)
    {
        if (Child == Parent)
        {
            LOG_ERROR("Cannot parent an entity to itself!");
            return;
        }

        if (Child == ECS::NullEntity)
        {
            LOG_ERROR("Cannot parent a null entity!");
            return;
        }

        if (Parent != ECS::NullEntity && Registry.GetHierarchy().IsDescendantOf(Parent, Child))
        {
            LOG_ERROR("Cannot create circular hierarchy - parent is a descendant of child!");
            return;
        }

        STransformComponent& ChildTransform = Registry.Get<STransformComponent>(Child);

        if (Registry.GetHierarchy().GetParent(Child) == Parent)
        {
            return;
        }

        FTransform NewLocalTransform;
        if (bPreserveWorld)
        {
            const FMatrix4 ChildWorldMatrix  = ChildTransform.GetWorldMatrix();
            const FMatrix4 ParentWorldMatrix = (Parent != ECS::NullEntity)
                ? Registry.Get<STransformComponent>(Parent).GetWorldMatrix() : FMatrix4(1.0f);
            const FMatrix4 NewLocalMatrix = Math::Inverse(ParentWorldMatrix) * ChildWorldMatrix;

            FVector3 Translation, Scale, Skew;
            FQuat    Rotation;
            FVector4 Perspective;
            Math::Decompose(NewLocalMatrix, Scale, Rotation, Translation, Skew, Perspective);
            NewLocalTransform.SetLocation(Translation);
            NewLocalTransform.SetRotation(Rotation);
            NewLocalTransform.SetScale(Scale);
        }

        RemoveFromParent(Registry, Child);

        if (Parent != ECS::NullEntity)
        {
            Registry.AttachChild(Child, Parent);
        }

        if (Parent != ECS::NullEntity && Registry.HasAny<SDisabledTag>(Parent))
        {
            if (!Registry.HasAny<SDisabledTag>(Child))
            {
                Registry.Emplace<SDisabledTag>(Child);
            }
        }

        if (bPreserveWorld)
        {
            ChildTransform.SetLocalTransform(NewLocalTransform); // marks the transform dirty
        }
        else
        {
            // Keep the replicated local; recompose world under the new parent next resolve.
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Child);
        }

        // An attachment change may have to reach clients, which is the netcode's business.
        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            if (CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>())
            {
                NetRuntime->OnEntityAttachmentChanged(*WorldPtr, Child);
            }
        }
    }

    void DestroyEntityHierarchy(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
        {
            return;
        }

        // Children before their parents, so a registry that orphans on destroy never sees one outlive its parent.
        TVector<ECS::FEntity> Subtree;
        Registry.GetHierarchy().ForEachDescendant(Entity, [&](ECS::FEntity Descendant) { Subtree.push_back(Descendant); });
        for (size_t Index = Subtree.size(); Index-- > 0;)
        {
            if (Registry.IsValid(Subtree[Index]))
            {
                Registry.Destroy(Subtree[Index]);
            }
        }
        Registry.Destroy(Entity);
    }

    void DetachImmediateChildren(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        Registry.GetHierarchy().ForEachChild(Entity, [&](ECS::FEntity Child)
        {
            RemoveFromParent(Registry, Child);
        });
    }

    void RemoveFromParent(ECS::FRegistry& Registry, ECS::FEntity Child)
    {
        if (Registry.GetHierarchy().GetParent(Child) == ECS::NullEntity)
        {
            return;
        }

        // Snapshot the world transform while the parent chain is intact; see SetEntityWorldTransform.
        FTransform WorldSnapshot;
        bool bHasTransform = false;
        if (STransformComponent* TransformComponent = Registry.TryGet<STransformComponent>(Child))
        {
            WorldSnapshot = TransformComponent->GetWorldTransform();
            bHasTransform = true;
        }

        Registry.DetachFromParent(Child);

        // Bake the snapshot back as local so the detached entity keeps its world placement.
        if (bHasTransform)
        {
            SetEntityWorldTransform(Registry, Child, WorldSnapshot);
        }
    }

    bool HasComponent(ECS::FRegistry& Registry, ECS::FEntity Entity, const CStruct* Type)
    {
        const FComponentOps* Ops = Type != nullptr ? Type->GetComponentOps() : nullptr;
        return Ops != nullptr && Ops->Has(Registry, Entity) != 0;
    }
    
    // bAnyDirty comes from the base, which lives in the header so a component can read it inline.
    struct CACHE_ALIGN FTransformDirtyState : FTransformDirtyGate
    {
        // The lock-free queue lets setters on any thread enqueue and stay SuppressGCTransition-safe.
        using FDirtyQueue = TConcurrentQueue<ECS::FEntity>;

        FDirtyQueue       DirtyTransforms;     // entities whose local transform changed (drained at resolve)
        FDirtyQueue       DirtyBodies;         // setter-moved entities to re-sync to physics (drained pre-sync)
        FFiberMutex       ResolveGuard;        // one resolver writes WorldTransform at a time (fiber-aware)

        // Resolve scratch reused across calls; HierBySlot lets the hierarchical filter parallelize.
        TVector<ECS::FEntity>     DrainScratch;
        TVector<TVector<uint32>>  HierBySlot;

        // Preorder slots of the dirty hierarchical entities, then the subtree ranges that cover them.
        TVector<uint32>               HierScratch;
        TVector<uint32>               RangeStarts;
        TVector<uint8>                SlotMarks;
        TVector<STransformComponent*> SlotTransforms;

        // Published moved-transform channel, off until a consumer opts in; duplicates are fine.
        std::atomic<bool> bPublishMoved{ false };
        FDirtyQueue       MovedTransforms;

        // One buffer per thread slot, since thousands of concurrent enqueues from a parallel pose overflow the ring into its spill lock.
        struct alignas(64) FMovedSlot
        {
            FMutex                Lock;
            TVector<ECS::FEntity> Items;
        };
        TVector<TUniquePtr<FMovedSlot>> MovedSlots;
        uint32                          NumMovedSlots = 0;

        FORCEINLINE void PublishMoved(ECS::FEntity Entity)
        {
            if (!bPublishMoved.load(std::memory_order_relaxed))
            {
                return;
            }

            const uint32 Slot = Jobs::IsInitialized() ? Jobs::GetWorkerIndex() : Constants::kIndexNoneU32;
            if (Slot < NumMovedSlots)
            {
                FMovedSlot& Moved = *MovedSlots[Slot];
                TScopeLock<FMutex> Guard(Moved.Lock);
                Moved.Items.push_back(Entity);
                return;
            }

            MovedTransforms.Enqueue(Entity);
        }

        void PublishMovedBulk(const ECS::FEntity* Entities, size_t Count)
        {
            if (Count == 0 || !bPublishMoved.load(std::memory_order_relaxed))
            {
                return;
            }

            const uint32 Slot = Jobs::IsInitialized() ? Jobs::GetWorkerIndex() : Constants::kIndexNoneU32;
            if (Slot < NumMovedSlots)
            {
                FMovedSlot& Moved = *MovedSlots[Slot];
                TScopeLock<FMutex> Guard(Moved.Lock);
                Moved.Items.insert(Moved.Items.end(), Entities, Entities + Count);
                return;
            }

            MovedTransforms.EnqueueBulk(Entities, Count);
        }

        void DrainMovedSlots(TVector<ECS::FEntity>& Out)
        {
            for (uint32 Slot = 0; Slot < NumMovedSlots; ++Slot)
            {
                FMovedSlot& Moved = *MovedSlots[Slot];
                TScopeLock<FMutex> Guard(Moved.Lock);
                Out.insert(Out.end(), Moved.Items.begin(), Moved.Items.end());
                Moved.Items.clear();
            }
        }

        FTransformDirtyState()
        {
            const uint32 Slots = Jobs::IsInitialized() ? Jobs::GetNumThreadSlots() : 1;
            HierBySlot.resize(Slots);
            NumMovedSlots = Slots;
            MovedSlots.reserve(Slots);
            for (uint32 Slot = 0; Slot < Slots; ++Slot)
            {
                MovedSlots.push_back(MakeUnique<FMovedSlot>());
            }
        }
    };
    
    static void OnTransformDirtied(FTransformDirtyState* State, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity))
        {
            if (!Transform->bWorldDirty)
            {
                Transform->bWorldDirty = true;
                State->DirtyTransforms.Enqueue(Entity);
            }
            
            if (!Transform->bBodyDirtyQueued && Registry.HasAny<SRigidBodyComponent, SCharacterPhysicsComponent>(Entity))
            {
                Transform->bBodyDirtyQueued = true;
                State->DirtyBodies.Enqueue(Entity);
            }
        }
        State->bAnyDirty.store(true, std::memory_order_release);
    }

    static void OnHierarchyChanged(FTransformDirtyState* State, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity))
        {
            Transform->bIsFlat = IsEntityTransformFlat(Registry, Entity);
        }
        OnTransformDirtied(State, Registry, Entity);
    }

    FTransformDirtyState* EnsureTransformDirtyState(ECS::FRegistry& Registry)
    {
        if (TUniquePtr<FTransformDirtyState>* Holder = Registry.Ctx().Find<TUniquePtr<FTransformDirtyState>>())
        {
            return Holder->get();
        }

        FTransformDirtyState& State = *Registry.Ctx().Emplace<TUniquePtr<FTransformDirtyState>>(MakeUnique<FTransformDirtyState>());
        Registry.GetSignals<FNeedsTransformUpdate>().OnConstruct.Connect<&OnTransformDirtied>(&State);
        Registry.OnHierarchyChanged().Connect<&OnHierarchyChanged>(&State);
        return &State;
    }

    FTransformDirtyGate* EnsureTransformDirtyGate(ECS::FRegistry& Registry)
    {
        return EnsureTransformDirtyState(Registry);
    }

    bool IsEntityTransformFlat(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return !Registry.GetHierarchy().IsLinked(Entity);
    }

    void PublishFlatMove(FTransformDirtyGate* Gate, ECS::FEntity Entity, bool bPublish, bool bQueueBody)
    {
        if (Gate == nullptr)
        {
            return;
        }

        // Single, non-virtual base, so the gate IS the state seen through the part the header knows.
        FTransformDirtyState* State = static_cast<FTransformDirtyState*>(Gate);

        if (bQueueBody)
        {
            State->DirtyBodies.Enqueue(Entity);
        }

        if (bPublish)
        {
            State->PublishMoved(Entity);
        }
    }

    void QueueDirtyTransform(FTransformDirtyGate* Gate, ECS::FEntity Entity, bool bQueueTransform, bool bQueueBody)
    {
        if (Gate == nullptr)
        {
            return;
        }

        FTransformDirtyState* State = static_cast<FTransformDirtyState*>(Gate);

        if (bQueueTransform)
        {
            State->DirtyTransforms.Enqueue(Entity);
        }
        if (bQueueBody)
        {
            State->DirtyBodies.Enqueue(Entity);
        }
        // Read before writing, since parallel writers storing to one line every call serialize on it.
        if (!State->bAnyDirty.load(std::memory_order_relaxed))
        {
            State->bAnyDirty.store(true, std::memory_order_relaxed);
        }
    }

    void QueueDirtyTransforms(FTransformDirtyGate* Gate, const ECS::FEntity* Entities, size_t Count)
    {
        if (Gate == nullptr || Count == 0)
        {
            return;
        }

        FTransformDirtyState* State = static_cast<FTransformDirtyState*>(Gate);
        State->DirtyTransforms.EnqueueBulk(Entities, Count);
        State->bAnyDirty.store(true, std::memory_order_relaxed);
    }

    void FlushDirtyPhysicsBodies(ECS::FRegistry& Registry)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.Ctx().Find<TUniquePtr<FTransformDirtyState>>();
        FTransformDirtyState* State = Holder ? Holder->get() : nullptr;
        if (State == nullptr)
        {
            return;   // no dirty state yet -> no setter has moved anything, so no body to re-sync
        }

        // Single-threaded at the physics boundary; the emplace is the batched dirty-body mark.
        ECS::FEntity Batch[128];
        std::size_t Count;
        while ((Count = State->DirtyBodies.DequeueBulk(Batch, 128)) != 0)
        {
            for (std::size_t i = 0; i < Count; ++i)
            {
                const ECS::FEntity E = Batch[i];
                if (!Registry.IsValid(E))
                {
                    continue;
                }

                // Release the enqueue guard as the entry is consumed, so a setter after this drain re-queues.
                if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(E))
                {
                    Transform->bBodyDirtyQueued = false;
                }

                if (Registry.HasAny<SRigidBodyComponent, SCharacterPhysicsComponent>(E))
                {
                    Registry.EmplaceOrReplace<FNeedsPhysicsBodyUpdate>(E);
                }
            }
        }
    }

    bool AnyTransformsDirty(ECS::FRegistry& Registry)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.Ctx().Find<TUniquePtr<FTransformDirtyState>>();
        FTransformDirtyState* State = Holder ? Holder->get() : nullptr;
        return State != nullptr && State->bAnyDirty.load(std::memory_order_acquire);
    }

    FTransform ComputeWorldTransform(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const auto XFormStorage = Registry.FindStorage<STransformComponent>();
        if (!XFormStorage || !XFormStorage.Contains(Entity))
        {
            return FTransform();
        }
        const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();

        // Everything above the highest ancestor that moved still has a current cached world, so the walk stops being needed there.
        TFixedVector<ECS::FEntity, 64> Chain;
        int32 TopmostDirty = -1;

        ECS::FEntity Current = Entity;
        while (Current != ECS::NullEntity)
        {
            if (XFormStorage.Contains(Current) && XFormStorage.Get(Current).bWorldDirty)
            {
                TopmostDirty = (int32)Chain.size();
            }
            Chain.push_back(Current);

            const ECS::FEntity Parent = Hierarchy.GetParent(Current);
            if (Parent == ECS::NullEntity || !Registry.IsValid(Parent) || !XFormStorage.Contains(Parent))
            {
                break;
            }
            Current = Parent;
        }

        if (TopmostDirty < 0)
        {
            return XFormStorage.Get(Entity).GetWorldTransformCached();
        }

        FTransform World = XFormStorage.Get(Chain[TopmostDirty]).LocalTransform;
        if (TopmostDirty + 1 < (int32)Chain.size())
        {
            World = XFormStorage.Get(Chain[TopmostDirty + 1]).GetWorldTransformCached() * World;
        }
        for (int32 i = TopmostDirty - 1; i >= 0; --i)
        {
            World = World * XFormStorage.Get(Chain[i]).LocalTransform;
        }
        return World;
    }

    void SetPublishMovedTransforms(ECS::FRegistry& Registry, bool bEnable)
    {
        EnsureTransformDirtyState(Registry)->bPublishMoved.store(bEnable, std::memory_order_release);
    }

    void PublishMovedTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.Ctx().Find<TUniquePtr<FTransformDirtyState>>();
        if (Holder != nullptr && Holder->get() != nullptr)
        {
            (*Holder)->PublishMoved(Entity);
        }
    }

    bool DrainMovedTransforms(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Out)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.Ctx().Find<TUniquePtr<FTransformDirtyState>>();
        FTransformDirtyState* State = Holder ? Holder->get() : nullptr;
        if (State == nullptr)
        {
            return false;
        }

        const SIZE_T Before = Out.size();

        State->DrainMovedSlots(Out);

        ECS::FEntity Batch[256];
        std::size_t Count;
        while ((Count = State->MovedTransforms.DequeueBulk(Batch, 256)) != 0)
        {
            Out.insert(Out.end(), Batch, Batch + Count);
        }

        // Bumping the epoch opens the next publish window, so no clearing pass over drained entities.
        State->PublishEpoch.fetch_add(1, std::memory_order_relaxed);

        return Out.size() != Before;
    }

    void ResolveAllDirtyTransforms(ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<STransformComponent>()), true, "Write<STransformComponent>");
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<SystemResource::Hierarchy>()), false, "Read<SystemResource::Hierarchy>");

        FTransformDirtyState& DirtyState = *EnsureTransformDirtyState(Registry);
        FFiberScopeLock ResolveLock(DirtyState.ResolveGuard);   // one resolver writes WorldTransform at a time

        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();

        // Fold external FNeedsTransformUpdate tags into the queue; bWorldDirty dedups already-queued.
        for (ECS::FEntity Tagged : Registry.View<FNeedsTransformUpdate>())
        {
            if (STransformComponent* Found = TransformStorage.TryGet(Tagged))
            {
                STransformComponent& Xf = *Found;
                if (!Xf.bWorldDirty)
                {
                    Xf.bWorldDirty = true;
                    DirtyState.DirtyTransforms.Enqueue(Tagged);
                    DirtyState.bAnyDirty.store(true, std::memory_order_relaxed);  // ensure the drain runs
                }
            }
        }
        Registry.ClearComponent<FNeedsTransformUpdate>();

        if (!DirtyState.bAnyDirty.load(std::memory_order_acquire))
        {
            return;
        }

        // The raw drain is serial and cheap; the per-entity filter and flat resolve run in parallel.
        TVector<ECS::FEntity>& Raw = DirtyState.DrainScratch;
        Raw.clear();
        {
            ECS::FEntity Batch[256];
            std::size_t Count;
            while ((Count = DirtyState.DirtyTransforms.DequeueBulk(Batch, 256)) != 0)
            {
                Raw.insert(Raw.end(), Batch, Batch + Count);
            }
        }

        DirtyState.bAnyDirty.store(false, std::memory_order_release);

        if (Raw.empty())
        {
            return;
        }

        const bool bAnyHierarchy = Hierarchy.NumLinked() != 0;
        if (bAnyHierarchy)
        {
            Hierarchy.EnsureOrder();
        }

        for (TVector<uint32>& Slot : DirtyState.HierBySlot)
        {
            Slot.clear();
        }

        auto FilterChunk = [&](const Task::FParallelRange& Chunk)
        {
            uint32 Worker = Jobs::GetWorkerIndex();
            if (Worker >= DirtyState.HierBySlot.size()) { Worker = 0; }
            TVector<uint32>& WorkerSlots = DirtyState.HierBySlot[Worker];

            constexpr uint32 MovedBatch = 128;
            ECS::FEntity Moved[MovedBatch];
            uint32 NumMoved = 0;

            for (uint32 Index = Chunk.Start; Index < Chunk.End; ++Index)
            {
                const ECS::FEntity E = Raw[Index];
                STransformComponent* T = TransformStorage.TryGet(E);
                if (T == nullptr || !T->bWorldDirty)
                {
                    continue;
                }

                // An entity outside the preorder has no parent to compose with, whatever its flat bit says.
                if (const uint32 OrderSlot = bAnyHierarchy ? Hierarchy.GetSlotOfLive(E) : ECS::FHierarchy::NoSlot; OrderSlot != ECS::FHierarchy::NoSlot)
                {
                    WorkerSlots.push_back(OrderSlot);
                    continue;
                }
                T->WorldTransform = T->LocalTransform;
                T->bWorldDirty    = false;

                Moved[NumMoved++] = E;
                if (NumMoved == MovedBatch)
                {
                    DirtyState.PublishMovedBulk(Moved, NumMoved);
                    NumMoved = 0;
                }
            }
            DirtyState.PublishMovedBulk(Moved, NumMoved);
        };

        LUMINA_PROFILE_VALUE("Transform/Dirty", (int64)Raw.size());
        if (Raw.size() > 1000)
        {
            LUMINA_PROFILE_SECTION("FilterDirty");
            Task::ParallelFor((uint32)Raw.size(), FilterChunk);
        }
        else
        {
            FilterChunk(Task::FParallelRange{ 0, (uint32)Raw.size(), 0 });
        }

        TVector<uint32>& DirtySlots = DirtyState.HierScratch;
        DirtySlots.clear();
        {
            size_t Total = 0;
            for (const TVector<uint32>& Slot : DirtyState.HierBySlot)
            {
                Total += Slot.size();
            }

            DirtySlots.reserve(Total);
            for (const TVector<uint32>& Slot : DirtyState.HierBySlot)
            {
                DirtySlots.insert(DirtySlots.end(), Slot.begin(), Slot.end());
            }
        }

        if (DirtySlots.empty())
        {
            return;
        }

        const TVector<ECS::FEntity>& Order = Hierarchy.GetOrder();
        const TVector<uint32>& SubtreeSizes = Hierarchy.GetSubtreeSizes();
        const TVector<uint32>& ParentSlots  = Hierarchy.GetParentSlots();

        // Ascending slots make a dirty descendant of a dirty ancestor fall inside the ancestor's range.
        TVector<uint32>& RangeStarts = DirtyState.RangeStarts;
        RangeStarts.clear();
        uint32 RangeTotal = 0;

        constexpr size_t SortedDirtyRatio = 16;
        if (DirtySlots.size() * SortedDirtyRatio < Order.size())
        {
            std::sort(DirtySlots.begin(), DirtySlots.end());

            uint32 CoveredEnd = 0;
            for (const uint32 Slot : DirtySlots)
            {
                if (Slot < CoveredEnd)
                {
                    continue;
                }
                RangeStarts.push_back(Slot);
                CoveredEnd = Slot + SubtreeSizes[Slot];
                RangeTotal += SubtreeSizes[Slot];
            }
        }
        else
        {
            // Most of the tree is dirty, so one pass over a mark per slot beats sorting the slots.
            TVector<uint8>& Marks = DirtyState.SlotMarks;
            Marks.assign(Order.size(), 0);
            for (const uint32 Slot : DirtySlots)
            {
                Marks[Slot] = 1;
            }

            const uint32 NumSlots = (uint32)Order.size();
            for (uint32 Slot = 0; Slot < NumSlots;)
            {
                if (Marks[Slot] == 0)
                {
                    ++Slot;
                    continue;
                }
                RangeStarts.push_back(Slot);
                RangeTotal += SubtreeSizes[Slot];
                Slot += SubtreeSizes[Slot];
            }
        }

        TVector<STransformComponent*>& SlotTransforms = DirtyState.SlotTransforms;
        if (SlotTransforms.size() < Order.size())
        {
            SlotTransforms.resize(Order.size());
        }

        auto ResolveRange = [&](uint32 RangeIndex)
        {
            const uint32 Begin = RangeStarts[RangeIndex];
            const uint32 End   = Begin + SubtreeSizes[Begin];

            // Every parent inside the range was written earlier in this walk, so only the head looks outside it.
            const uint32 HeadParent = ParentSlots[Begin];
            const STransformComponent* HeadParentTransform = HeadParent != ECS::FHierarchy::NoSlot ? TransformStorage.TryGet(Order[HeadParent]) : nullptr;

            for (uint32 Slot = Begin; Slot < End; ++Slot)
            {
                const ECS::FEntity Entity = Order[Slot];
                STransformComponent* T = TransformStorage.TryGet(Entity);
                SlotTransforms[Slot] = T;
                if (T == nullptr)
                {
                    continue;
                }

                const FTransform* ParentWorld = nullptr;
                if (Slot == Begin)
                {
                    ParentWorld = HeadParentTransform != nullptr ? &HeadParentTransform->GetWorldTransformCached() : nullptr;
                }
                else if (const STransformComponent* InRange = SlotTransforms[ParentSlots[Slot]])
                {
                    ParentWorld = &InRange->WorldTransform;
                }

                T->WorldTransform = ParentWorld != nullptr ? *ParentWorld * T->LocalTransform : T->LocalTransform;
                T->bWorldDirty    = false;
            }
        };

        // Ranges are contiguous runs of the preorder, so a chunk publishes them in batches rather than one lock each.
        auto ResolveRangeChunk = [&](const Task::FParallelRange& Chunk)
        {
            constexpr uint32 MovedBatch = 128;
            ECS::FEntity Moved[MovedBatch];
            uint32 NumMoved = 0;

            for (uint32 RangeIndex = Chunk.Start; RangeIndex < Chunk.End; ++RangeIndex)
            {
                ResolveRange(RangeIndex);

                const uint32 Begin = RangeStarts[RangeIndex];
                const uint32 Count = SubtreeSizes[Begin];
                if (NumMoved + Count > MovedBatch)
                {
                    DirtyState.PublishMovedBulk(Moved, NumMoved);
                    NumMoved = 0;
                }
                if (Count > MovedBatch)
                {
                    DirtyState.PublishMovedBulk(&Order[Begin], Count);
                    continue;
                }
                std::copy_n(&Order[Begin], Count, Moved + NumMoved);
                NumMoved += Count;
            }
            DirtyState.PublishMovedBulk(Moved, NumMoved);
        };

        LUMINA_PROFILE_VALUE("Transform/Ranges", (int64)RangeStarts.size());
        LUMINA_PROFILE_VALUE("Transform/RangeEntities", (int64)RangeTotal);
        if (RangeTotal > 1000 && RangeStarts.size() > 1)
        {
            LUMINA_PROFILE_SECTION("ResolveRanges");
            Task::ParallelFor((uint32)RangeStarts.size(), ResolveRangeChunk);
        }
        else
        {
            ResolveRangeChunk(Task::FParallelRange{ 0, (uint32)RangeStarts.size(), 0 });
        }
    }

    // --- Entity transform accessors ---

    FQuat GetEntityRotation(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        auto* Transform = Registry.TryGet<STransformComponent>(Entity);
        return Transform ? Transform->GetWorldRotation() : FQuat{};
    }

    FVector3 GetEntityScale(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        auto* Transform = Registry.TryGet<STransformComponent>(Entity);
        return Transform ? Transform->GetWorldScale() : FVector3{};
    }

    void SetEntityScale(ECS::FRegistry& Registry, ECS::FEntity Entity, const FVector3& Scale)
    {
        if (auto* Transform = Registry.TryGet<STransformComponent>(Entity))
        {
            Transform->SetScale(Scale);
        }
    }

    namespace
    {
        // Remap in place through Map, or clear to null when bClearUnmapped; ECS::NullEntity is left alone.
        void RemapEntityHandle(uint32& Value, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped)
        {
            const ECS::FEntity Stored = static_cast<ECS::FEntity>(Value);
            if (Stored == ECS::NullEntity)
            {
                return;
            }

            auto It = Map.find(Stored);
            if (It != Map.end())
            {
                Value = static_cast<uint32>((It->second).Value);
            }
            else if (bClearUnmapped)
            {
                Value = static_cast<uint32>((static_cast<ECS::FEntity>(ECS::NullEntity)).Value);
            }
        }

        // One pass over the flattened list, NOT one per super, since Link already folded the supers in.
        template<typename Visitor>
        void ForEachEntityRefInStruct(CStruct* Struct, void* Data, Visitor& Visit)
        {
            for (FProperty* Property : Struct->GetProperties())
            {
                if (Property->IsA(EPropertyTypeFlags::Entity))
                {
                    uint32 Value = 0;
                    Property->GetValue(Data, &Value);
                    Visit(Value);
                    Property->SetValue(Data, Value);
                }
                else if (Property->IsA(EPropertyTypeFlags::Struct))
                {
                    if (CStruct* Inner = static_cast<FStructProperty*>(Property)->GetStruct())
                    {
                        ForEachEntityRefInStruct(Inner, Property->GetValuePtr<void>(Data), Visit);
                    }
                }
                else if (Property->IsA(EPropertyTypeFlags::Vector))
                {
                    FArrayProperty* ArrayProperty = static_cast<FArrayProperty*>(Property);
                    FProperty* Inner = ArrayProperty->GetInternalProperty();
                    if (Inner == nullptr)
                    {
                        continue;
                    }

                    void* ArrayPtr = Property->GetValuePtr<void>(Data);

                    if (Inner->IsA(EPropertyTypeFlags::Entity))
                    {
                        ArrayProperty->ForEach<uint32>(ArrayPtr, [&](uint32* Elem, SIZE_T)
                        {
                            Visit(*Elem);
                        });
                    }
                    else if (Inner->IsA(EPropertyTypeFlags::Struct))
                    {
                        if (CStruct* ElemStruct = static_cast<FStructProperty*>(Inner)->GetStruct())
                        {
                            ArrayProperty->ForEach(ArrayPtr, [&](void* Elem, SIZE_T)
                            {
                                ForEachEntityRefInStruct(ElemStruct, Elem, Visit);
                            });
                        }
                    }
                }
            }
        }

        template<typename Visitor>
        void ForEachEntityRefOnEntity(ECS::FRegistry& Registry, ECS::FEntity Entity, Visitor& Visit)
        {
            for (Lumina::ECS::FSparseSet* StoragePtr : Registry.GetActiveStorages())
            {
                const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
                Lumina::ECS::FSparseSet& Storage = *StoragePtr;
                if (!Storage.Contains(Entity))
                {
                    continue;
                }

                if (CStruct* Struct = FindComponentStructByTypeId(ID))
                {
                    ForEachEntityRefInStruct(Struct, Storage.GetRaw(Entity), Visit);
                }
            }

            // A script is held BY a component, so its reflected properties are invisible to the storage walk.
            if (SEntityScriptComponent* Scripts = Registry.TryGet<SEntityScriptComponent>(Entity))
            {
                for (const TStrongObjectPtr<CEntityScript>& Held : Scripts->Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (Script != nullptr && Script->GetClass() != nullptr)
                    {
                        ForEachEntityRefInStruct(Script->GetClass(), Script, Visit);
                    }
                }
            }
        }
    }

    // --- Reflection / queries + misc ---

    void RemapEntityReferences(ECS::FRegistry& Registry, ECS::FEntity Entity, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped)
    {
        auto Visit = [&](uint32& Value)
        {
            RemapEntityHandle(Value, Map, bClearUnmapped);
        };
        ForEachEntityRefOnEntity(Registry, Entity, Visit);
    }

    void CollectEntityReferences(ECS::FRegistry& Registry, ECS::FEntity Entity, TVector<ECS::FEntity>& Out)
    {
        auto Visit = [&](uint32& Value)
        {
            const ECS::FEntity Referenced = static_cast<ECS::FEntity>(Value);
            if (Referenced != ECS::NullEntity)
            {
                Out.push_back(Referenced);
            }
        };
        ForEachEntityRefOnEntity(Registry, Entity, Visit);
    }

    void SetEntityWorldRotationCached(ECS::FRegistry& Registry, ECS::FEntity Entity, const FQuat& WorldRotation)
    {
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<STransformComponent>()), true, "Write<STransformComponent>");
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<SystemResource::Hierarchy>()), false, "Read<SystemResource::Hierarchy>");

        STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        FQuat LocalRotation = WorldRotation;
        if (const ECS::FEntity ParentEntity = Registry.GetHierarchy().GetParent(Entity); ParentEntity != ECS::NullEntity)
        {
            if (const STransformComponent* Parent = Registry.TryGet<STransformComponent>(ParentEntity))
            {
                LocalRotation = Math::Normalize(Math::Inverse(Parent->ComputeWorldTransform().GetRotation()) * WorldRotation);
            }
        }

        if (Transform->LocalTransform.GetRotation() == LocalRotation && !Transform->bHasPhysicsBody)
        {
            return;
        }
        FTransform NewLocal = Transform->LocalTransform;
        NewLocal.SetRotation(LocalRotation);
        Transform->SetLocalTransform(NewLocal);
    }

    void SetEntityWorldTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FTransform& WorldTransform)
    {
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<STransformComponent>()), true, "Write<STransformComponent>");
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<SystemResource::Hierarchy>()), false, "Read<SystemResource::Hierarchy>");

        STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        FMatrix4 ParentWorldMatrix(1.0f);
        if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity); Parent != ECS::NullEntity)
        {
            if (const STransformComponent* ParentTransform = Registry.TryGet<STransformComponent>(Parent))
            {
                ParentWorldMatrix = ParentTransform->GetWorldMatrix();
            }
        }

        FMatrix4 LocalMatrix = Math::Inverse(ParentWorldMatrix) * WorldTransform.GetMatrix();

        FVector3 Translation, Scale, Skew;
        FQuat Rotation;
        FVector4 Perspective;
        Math::Decompose(LocalMatrix, Scale, Rotation, Translation, Skew, Perspective);

        FTransform NewLocal;
        NewLocal.SetLocation(Translation);
        NewLocal.SetRotation(Rotation);
        NewLocal.SetScale(Scale);

        // Gameplay that re-asserts a pose every frame would otherwise re-sync the entity and its subtree each time.
        if (NewLocal == Transform->LocalTransform && !Transform->bHasPhysicsBody)
        {
            return;
        }
        Transform->SetLocalTransform(NewLocal);
    }

    void DestroyEntity(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        Registry.Destroy(Entity);
    }

    void SetEntityBodyType(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        Registry.EmplaceOrReplace<FNeedsPhysicsBodyUpdate>(Entity);
    }

    // Tag a body for the physics sync to reposition it. Single-threaded path (bodies aren't mass-moved).
    void MarkPhysicsBodyDirtyIfBodied(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (Registry.HasAny<SRigidBodyComponent, SCharacterPhysicsComponent>(Entity))
        {
            Registry.EmplaceOrReplace<FNeedsPhysicsBodyUpdate>(Entity);
        }
    }

    // External (non-setter) dirtying. The tag flows through OnTransformDirtied to set the flag. Single-threaded.
    void MarkTransformDirty(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
        MarkPhysicsBodyDirtyIfBodied(Registry, Entity);
    }
}
