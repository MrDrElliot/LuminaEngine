#include "RuntimePCH.h"
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
#include "Memory/MemoryConcurrentQueue.h"
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
#include "World/World.h"
#include "World/WorldContext.h"
#include <atomic>
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        THashMap<uint32, const FComponentOps*>& ComponentOpsMap()
        {
            static THashMap<uint32, const FComponentOps*> Map;
            return Map;
        }

        uint32 ComponentOpsKey(FStringView Name)
        {
            const FString Terminated(Name.data(), Name.size());
            return entt::hashed_string(Terminated.c_str());
        }
    }

    namespace
    {
        THashMap<uint32, FString>& AccessTypeNameMap()
        {
            static THashMap<uint32, FString> Map;
            return Map;
        }
    }

    void RegisterAccessTypeName(uint32 Id, FStringView Name)
    {
        AccessTypeNameMap()[Id] = FString(Name.data(), Name.size());
    }

    const char* GetAccessTypeName(uint32 Id)
    {
        // Lazily register the non-reflected SystemResource:: tags (they have no op table to hook).
        auto& Map = AccessTypeNameMap();
        if (Map.empty() || Map.find(static_cast<uint32>(entt::type_hash<SystemResource::PhysicsQuery>::value())) == Map.end())
        {
            Map[static_cast<uint32>(entt::type_hash<SystemResource::PhysicsQuery>::value())]    = "PhysicsQuery";
            Map[static_cast<uint32>(entt::type_hash<SystemResource::EntityStructure>::value())] = "EntityStructure";
            Map[static_cast<uint32>(entt::type_hash<SystemResource::EventDispatcher>::value())] = "EventDispatcher";
        }
        const auto It = Map.find(Id);
        return It != Map.end() ? It->second.c_str() : nullptr;
    }

    void RegisterComponentOps(FStringView Name, const FComponentOps* Ops)
    {
        ComponentOpsMap()[ComponentOpsKey(Name)] = Ops;
        if (Ops != nullptr)
        {
            RegisterAccessTypeName(static_cast<uint32>(Ops->TypeId), Name);
        }
    }

    const FComponentOps* FindComponentOps(FStringView Name)
    {
        const auto It = ComponentOpsMap().find(ComponentOpsKey(Name));
        return It != ComponentOpsMap().end() ? It->second : nullptr;
    }

    //~ Honest-access validation (see SystemAccess.h). One thread_local per Runtime thread holds the access
    //  of the system currently ticking on that thread; the scheduler binds/unbinds it around each Update.
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

        // Log each unique (system access, missing access) once so a per-entity loop doesn't spam, then assert
        // (debug only). The same access object pointer identifies the offending system for the frame.
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
            LOG_ERROR("System ran in parallel but under-declared its ECS access: it touched something requiring "
                      "{} which it did not declare. Add it to the system's FSystemAccess (or drop the Access member "
                      "to run exclusive). This is a silent data race under concurrent scheduling.", What);
            DEBUG_ASSERT(false, "System under-declared ECS access (see log).");
        }
    }
#endif
}

using namespace entt::literals; 

namespace Lumina::ECS::Utils
{
    // --- Serialization ---

    /**
     * The reflected component types present in a registry, resolved ONCE.
     *
     * The write path has to ask, for each entity, which components it carries -- and entt offers no
     * "components of entity X", so the storages must be probed. That probe is inherent. What was NOT
     * inherent is that every probe HIT then paid `entt::resolve(Set.info())` plus an `InvokeMetaFunc`
     * to recover the CStruct*, and both are hash lookups into entt's meta registry. Those depend only on
     * the component TYPE, so paying them per component INSTANCE meant a whole-registry save spent
     * (entities x components) on meta traffic alone. On the editor's undo snapshot -- which serializes
     * the entire registry twice per transaction -- that was the bulk of a multi-hundred-millisecond stall.
     *
     * Empty storages are dropped here too: an assured-but-unused pool still cost a contains() probe per
     * entity, and a mature registry has a lot of them.
     *
     * Built in `Registry.storage()` order, so the emitted component order is unchanged -- which matters,
     * because the undo system byte-compares two captures to decide whether a transaction was a no-op.
     */
    struct FComponentTypeCache
    {
        struct FEntry
        {
            entt::sparse_set*   Set;
            CStruct*            Struct;
        };
        TVector<FEntry> Entries;

        void Build(FEntityRegistry& Registry)
        {
            Entries.clear();
            for (auto&& Curr : Registry.storage())
            {
                entt::sparse_set& Set = Curr.second;
                if (Set.empty())
                {
                    continue;
                }

                entt::meta_type MetaType = entt::resolve(Set.info());
                if (entt::meta_any ReturnValue = InvokeMetaFunc(MetaType, "static_struct"_hs))
                {
                    CStruct* StructType = ReturnValue.cast<CStruct*>();
                    ASSERT(StructType);
                    Entries.push_back({ &Set, StructType });
                }
            }
        }
    };

    // Internal write path. Cache may be null, in which case one is built for this entity alone -- correct
    // but pointless for a bulk save, which is why SerializeRegistry builds it once and passes it down.
    static bool SerializeEntityWrite(FArchive& RESTRICT Ar, FEntityRegistry& RESTRICT Registry,
                                     entt::entity& RESTRICT Entity, const FComponentTypeCache* Cache);

    bool SerializeEntity(FArchive& RESTRICT Ar, FEntityRegistry& RESTRICT Registry, entt::entity& RESTRICT Entity)
    {
        using namespace entt::literals;

        if (Ar.IsWriting())
        {
            return SerializeEntityWrite(Ar, Registry, Entity, nullptr);
        }
        else if (Ar.IsReading())
        {
            Ar << Entity;

            if (!Registry.valid(Entity))
            {
                entt::entity New = Registry.create(Entity);
                ALERT_IF_NOT(New == Entity);
                Entity = New;
            }

            bool bHasRelationship = false;
            Ar << bHasRelationship;

            if (bHasRelationship)
            {
                FRelationshipComponent& RelationshipComponent = Registry.emplace_or_replace<FRelationshipComponent>(Entity);
                Ar << RelationshipComponent;
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
                    if (Struct == STagComponent::StaticStruct())
                    {
                        STagComponent NewTagComponent;
                        Struct->SerializeTaggedProperties(Ar, &NewTagComponent);
                        auto HashedString = entt::hashed_string(NewTagComponent.Tag.c_str());

                        if (!Registry.storage<STagComponent>(HashedString).contains(Entity))
                        {
                            Registry.storage<STagComponent>(HashedString).emplace(Entity, NewTagComponent);
                        }
                    }
                    else
                    {
                        entt::hashed_string HashString(Struct->GetName().c_str());
                        if (entt::meta_type Meta = entt::resolve(HashString))
                        {
                            entt::meta_any Any = Meta.construct();

                            InvokeMetaFunc(Meta, "serialize"_hs, entt::forward_as_meta(Ar), entt::forward_as_meta(Any));
                            InvokeMetaFunc(Meta, "emplace"_hs, entt::forward_as_meta(Registry), Entity, entt::forward_as_meta(Any));
                        }
                        else
                        {
                            LOG_WARN("[ECS] Entity {}: component '{}' has CStruct but no entt meta_type; skipping ({} bytes).",
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

    static bool SerializeEntityWrite(FArchive& RESTRICT Ar, FEntityRegistry& RESTRICT Registry,
                                     entt::entity& RESTRICT Entity, const FComponentTypeCache* Cache)
    {
        Ar << Entity;

        FRelationshipComponent* RelationshipComponent = Registry.try_get<FRelationshipComponent>(Entity);
        bool bHasRelationship = (RelationshipComponent != nullptr);
        Ar << bHasRelationship;

        if (bHasRelationship)
        {
            Ar << *RelationshipComponent;
        }

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
            if (!Entry.Set->contains(Entity))
            {
                continue;
            }

            FName Name = Entry.Struct->GetName();
            Ar << Name;

            int64 ComponentStart = Ar.Tell();

            int64 ComponentSize = 0;
            Ar << ComponentSize;

            int64 StartOfComponentData = Ar.Tell();

            Entry.Struct->SerializeTaggedProperties(Ar, Entry.Set->value(Entity));

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

    bool SerializeRegistry(FArchive& Ar, FEntityRegistry& Registry)
    {
        using namespace entt::literals;
        
        if (Ar.IsWriting())
        {
            Registry.compact<>();
            auto View = Registry.view<entt::entity>(entt::exclude<FEditorComponent>);

            // ONCE for the whole walk, not once per entity. See FComponentTypeCache -- this is what turns
            // the write from (entities x component types) meta-registry lookups into (entities x populated
            // types) plain sparse probes.
            FComponentTypeCache TypeCache;
            TypeCache.Build(Registry);

            int64 PreSerializePos = Ar.Tell();

            int32 NumEntitiesSerialized = 0;
            Ar << NumEntitiesSerialized;

            View.each([&](entt::entity Entity)
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

            for (int32 i = 0; i < NumEntitiesSerialized; ++i)
            {
                int64 EntitySaveSize = 0;
                Ar << EntitySaveSize;

                int64 PreEntityPos = Ar.Tell();

                entt::entity NewEntity = entt::null;
                bool bSuccess = ECS::Utils::SerializeEntity(Ar, Registry, NewEntity);

                // Clear the per-entity error so one corrupt entity doesn't poison subsequent
                // SerializeEntity calls; the size-header seek below re-aligns the stream regardless.
                Ar.SetHasError(false);

                if (!bSuccess || NewEntity == entt::null)
                {
                    // Skip to the next entity using the saved size
                    LOG_ERROR("Failed to serialize entity: {}", (int)NewEntity);
                    Ar.Seek(PreEntityPos + EntitySaveSize);
                    continue;
                }

                Registry.emplace_or_replace<FNeedsTransformUpdate>(NewEntity);

                int64 PostEntityPos = Ar.Tell();
                int64 ActualBytesRead = PostEntityPos - PreEntityPos;

                if (ActualBytesRead != EntitySaveSize)
                {
                    // Data mismatch, seek to correct position to stay aligned
                    LOG_ERROR("Entity Serialization Mismatch For {}: Expected: {} - Read: {}", (int)NewEntity, EntitySaveSize, ActualBytesRead);
                    Ar.Seek(PreEntityPos + EntitySaveSize);
                }
            }
        }

        return !Ar.HasError();
    }

    bool EntityHasTag(const FName& Tag, FEntityRegistry& Registry, entt::entity Entity)
    {
        return Registry.storage<STagComponent>(entt::hashed_string(Tag.c_str())).contains(Entity);
    }
    
    // --- Hierarchy ---

    void AddToParent(FEntityRegistry& Registry, entt::entity Child, entt::entity Parent)
    {
        FRelationshipComponent& ChildRelationship = Registry.get_or_emplace<FRelationshipComponent>(Child);
        FRelationshipComponent& ParentRelationship = Registry.get_or_emplace<FRelationshipComponent>(Parent);

        ChildRelationship.Parent = Parent;

        ChildRelationship.Prev = entt::null;
        ChildRelationship.Next = ParentRelationship.First;

        if (ParentRelationship.First != entt::null)
        {
            FRelationshipComponent& OldFirstRelationship = Registry.get<FRelationshipComponent>(ParentRelationship.First);
            OldFirstRelationship.Prev = Child;
        }

        ParentRelationship.First = Child;
        ParentRelationship.Children++;
    }
    
    void ReparentEntity(FEntityRegistry& Registry, entt::entity Child, entt::entity Parent, bool bPreserveWorld)
    {
        // Self-parent or circular hierarchy causes an infinite loop in ForEachChild.
        if (Child == Parent)
        {
            LOG_ERROR("Cannot parent an entity to itself!");
            return;
        }

        if (Child == entt::null)
        {
            LOG_ERROR("Cannot parent a null entity!");
            return;
        }

        // Always guarded: a cycle here infinite-loops in ForEachChild traversal, so this
        // must reject in shipping builds too, not just debug.
        if (Parent != entt::null && IsDescendantOf(Registry, Parent, Child))
        {
            LOG_ERROR("Cannot create circular hierarchy - parent is a descendant of child!");
            return;
        }

        FRelationshipComponent& ChildRelationship = Registry.get_or_emplace<FRelationshipComponent>(Child);
        STransformComponent& ChildTransform = Registry.get<STransformComponent>(Child);

        if (ChildRelationship.Parent == Parent)
        {
            return;
        }
        
        FTransform NewLocalTransform;
        if (bPreserveWorld)
        {
            const FMatrix4 ChildWorldMatrix  = ChildTransform.GetWorldMatrix();
            const FMatrix4 ParentWorldMatrix = (Parent != entt::null)
                ? Registry.get<STransformComponent>(Parent).GetWorldMatrix() : FMatrix4(1.0f);
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

        if (Parent != entt::null)
        {
            AddToParent(Registry, Child, Parent);
        }
        else
        {
            ChildRelationship.Parent = entt::null;
        }

        if (Parent != entt::null && Registry.any_of<SDisabledTag>(Parent))
        {
            if (!Registry.any_of<SDisabledTag>(Child))
            {
                Registry.emplace<SDisabledTag>(Child);
            }
        }

        if (bPreserveWorld)
        {
            ChildTransform.SetLocalTransform(NewLocalTransform); // marks the transform dirty
        }
        else
        {
            // Keep the replicated local; recompose world under the new parent next resolve.
            Registry.emplace_or_replace<FNeedsTransformUpdate>(Child);
        }

        // An attachment change may have to reach clients. Whether it does, and what that costs, is
        // the netcode's business.
        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            if (CWorld** WorldPtr = Registry.ctx().find<CWorld*>())
            {
                NetRuntime->OnEntityAttachmentChanged(*WorldPtr, Child);
            }
        }
    }

    void DestroyEntityHierarchy(FEntityRegistry& Registry, entt::entity Entity)
    {
        if (Entity == entt::null || !Registry.valid(Entity))
        {
            return;
        }

        TVector<entt::entity> ToDestroy;
        CollectDescendants(Registry, Entity, ToDestroy);

        // Detach from the parent's sibling list first so the parent's relationship component
        // stops pointing at freed entities, then destroy this entity and all descendants.
        if (Registry.any_of<FRelationshipComponent>(Entity))
        {
            RemoveFromParent(Registry, Entity);
        }

        ToDestroy.push_back(Entity);

        for (entt::entity E : ToDestroy)
        {
            if (Registry.valid(E))
            {
                Registry.destroy(E);
            }
        }
    }

    void DetachImmediateChildren(FEntityRegistry& Registry, entt::entity Entity)
    {
        TVector<entt::entity> ToDestroy;
        CollectChildren(Registry, Entity, ToDestroy);
        
        for (auto It = ToDestroy.rbegin(); It != ToDestroy.rend(); ++It)
        {
            if (Registry.valid(*It))
            {
                RemoveFromParent(Registry, *It);
            }
        }
    }

    void RemoveFromParent(FEntityRegistry& Registry, entt::entity Child)
    {
        FRelationshipComponent* ChildRelationship = Registry.try_get<FRelationshipComponent>(Child);
        if (!ChildRelationship || ChildRelationship->Parent == entt::null)
        {
            return;
        }

        // Snapshot the world transform while the parent chain is still intact; once detached the
        // entity has no parent, so its local space becomes its world space (see SetEntityWorldTransform).
        FTransform WorldSnapshot;
        bool bHasTransform = false;
        if (STransformComponent* TransformComponent = Registry.try_get<STransformComponent>(Child))
        {
            WorldSnapshot = TransformComponent->GetWorldTransform();
            bHasTransform = true;
        }

        entt::entity OldParent = ChildRelationship->Parent;
        FRelationshipComponent* ParentRelationship = Registry.try_get<FRelationshipComponent>(OldParent);

        if (!ParentRelationship)
        {
            return;
        }

        ParentRelationship->Children--;

        if (ChildRelationship->Prev != entt::null)
        {
            FRelationshipComponent& PrevRelationship = Registry.get<FRelationshipComponent>(ChildRelationship->Prev);
            PrevRelationship.Next = ChildRelationship->Next;
        }
        else
        {
            ParentRelationship->First = ChildRelationship->Next;
        }

        if (ChildRelationship->Next != entt::null)
        {
            FRelationshipComponent& NextRelationship = Registry.get<FRelationshipComponent>(ChildRelationship->Next);
            NextRelationship.Prev = ChildRelationship->Prev;
        }

        ChildRelationship->Parent = entt::null;
        ChildRelationship->Prev = entt::null;
        ChildRelationship->Next = entt::null;

        // Bake the snapshot back as local now that there's no parent to inherit from, so the
        // detached entity keeps its world placement.
        if (bHasTransform)
        {
            SetEntityWorldTransform(Registry, Child, WorldSnapshot);
        }
    }

    bool IsDescendantOf(FEntityRegistry& Registry, entt::entity Potential, entt::entity Ancestor)
    {
        if (Potential == entt::null || Ancestor == entt::null)
        {
            return false;
        }

        entt::entity Current = Potential;
        while (Current != entt::null)
        {
            FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Current);
            if (!Relationship)
            {
                break;
            }

            if (Relationship->Parent == Ancestor)
            {
                return true;
            }

            Current = Relationship->Parent;
        }

        return false;
    }

    bool IsChild(FEntityRegistry& Registry, entt::entity Entity)
    {
        FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Entity);
        return Relationship ? Relationship->Parent != entt::null : false;
    }

    bool IsParent(FEntityRegistry& Registry, entt::entity Entity)
    {
        return GetChildCount(Registry, Entity) != 0;
    }

    entt::entity GetRootEntity(FEntityRegistry& Registry, entt::entity Entity)
    {
        entt::entity Current = Entity;
        while (Current != entt::null)
        {
            FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Current);
            if (!Relationship || Relationship->Parent == entt::null)
            {
                break;
            }

            Current = Relationship->Parent;
        }

        return Current;
    }

    size_t GetChildCount(FEntityRegistry& Registry, entt::entity Parent)
    {
        FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Parent);
        return Relationship ? Relationship->Children : 0;
    }

    void CollectDescendants(FEntityRegistry& Registry, entt::entity Entity, TVector<entt::entity>& OutDescendants)
    {
        OutDescendants.push_back(Entity);

        FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Entity);
        if (!Relationship || Relationship->First == entt::null)
        {
            return;
        }

        entt::entity Current = Relationship->First;
        while (Current != entt::null)
        {
            CollectDescendants(Registry, Current, OutDescendants);

            FRelationshipComponent* CurrentRelationship = Registry.try_get<FRelationshipComponent>(Current);
            Current = CurrentRelationship ? CurrentRelationship->Next : entt::null;
        }
    }

    void CollectChildren(FEntityRegistry& Registry, entt::entity Entity, TVector<entt::entity>& OutChildren)
    {
        FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Entity);
        if (!Relationship || Relationship->First == entt::null)
        {
            return;
        }

        entt::entity Current = Relationship->First;
        while (Current != entt::null)
        {
            OutChildren.push_back(Current);

            FRelationshipComponent* CurrentRelationship = Registry.try_get<FRelationshipComponent>(Current);
            Current = CurrentRelationship ? CurrentRelationship->Next : entt::null;
        }
    }

    bool HasComponent(FEntityRegistry& Registry, entt::entity Entity, entt::meta_type Type)
    {
        if (entt::meta_any Any = InvokeMetaFunc(Type, "has"_hs, entt::forward_as_meta(Registry), Entity))
        {
            return Any.cast<bool>();
        }
        
        return false;
    }
    
    // bAnyDirty comes from the base, which lives in the header so a component can read it inline.
    struct CACHE_ALIGN FTransformDirtyState : FTransformDirtyGate
    {
        // entt::entity is a trivially-copyable uint32; the lock-free queue lets setters on any thread
        // (worker fibers, Jolt's step jobs) enqueue without a lock, so they stay SuppressGCTransition-safe.
        using FDirtyQueue = moodycamel::ConcurrentQueue<entt::entity, Memory::FTrackedConcurrentQueueTraits>;

        FDirtyQueue       DirtyTransforms;     // entities whose local transform changed (drained at resolve)
        FDirtyQueue       DirtyBodies;         // setter-moved entities to re-sync to physics (drained pre-sync)
        FFiberMutex       ResolveGuard;        // one resolver writes WorldTransform at a time (fiber-aware)

        // Per-thread-slot producer tokens: skip moodycamel's per-enqueue implicit-producer hash lookup on
        // the hot setter path. Indexed by Jobs::GetWorkerIndex() -- one slot per thread, so a given token is
        // never used concurrently (the same invariant the job scheduler's own tokens rely on). Sized once at
        // construction; left empty if the scheduler isn't up yet, in which case enqueue falls back to implicit.
        TVector<moodycamel::ProducerToken> TransformTokens;
        TVector<moodycamel::ProducerToken> BodyTokens;

        // Resolve scratch, reused across calls (ResolveAllDirtyTransforms holds ResolveGuard, so single
        // owner). DrainScratch is the raw bulk-dequeued id list; HierBySlot collects the rare hierarchical
        // entities per worker slot during the parallel filter (so the filter itself parallelizes).
        TVector<entt::entity>          DrainScratch;
        TVector<TVector<entt::entity>> HierBySlot;

        // Merged HierBySlot, kept as a member so the resolve reuses one allocation across frames.
        TVector<entt::entity>          HierScratch;

        // Published "who moved" channel, off until a consumer opts in (SetPublishMovedTransforms).
        // Written from the resolve's ParallelFor bodies and from the lazy chain resolve, hence the
        // concurrent queue; drained on the game thread. Duplicates are fine -- the consumer is
        // idempotent per entity, and dedup here would cost more than it saves.
        std::atomic<bool> bPublishMoved{ false };
        FDirtyQueue       MovedTransforms;

        FORCEINLINE void PublishMoved(entt::entity Entity)
        {
            if (bPublishMoved.load(std::memory_order_relaxed))
            {
                MovedTransforms.enqueue(Entity);
            }
        }

        FTransformDirtyState()
        {
            const uint32 Slots = Jobs::IsInitialized() ? Jobs::GetNumThreadSlots() : 1;
            HierBySlot.resize(Slots);

            if (!Jobs::IsInitialized())
            {
                return;   // tokens stay empty -> implicit-producer fallback (always safe)
            }
            TransformTokens.reserve(Slots);
            BodyTokens.reserve(Slots);
            for (uint32 i = 0; i < Slots; ++i)
            {
                TransformTokens.emplace_back(DirtyTransforms);
                BodyTokens.emplace_back(DirtyBodies);
            }
        }
    };
    
    static void OnTransformDirtied(FTransformDirtyState* State, FEntityRegistry& Registry, entt::entity Entity)
    {
        if (STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
        {
            if (!Transform->bWorldDirty)
            {
                Transform->bWorldDirty = true;
                State->DirtyTransforms.enqueue(Entity);
            }
            
            if (!Transform->bBodyDirtyQueued && Registry.any_of<SRigidBodyComponent, SCharacterPhysicsComponent>(Entity))
            {
                Transform->bBodyDirtyQueued = true;
                State->DirtyBodies.enqueue(Entity);
            }
        }
        State->bAnyDirty.store(true, std::memory_order_release);
    }

    FTransformDirtyState* EnsureTransformDirtyState(FEntityRegistry& Registry)
    {
        if (TUniquePtr<FTransformDirtyState>* Holder = Registry.ctx().find<TUniquePtr<FTransformDirtyState>>())
        {
            return Holder->get();
        }

        FTransformDirtyState& State = *Registry.ctx().emplace<TUniquePtr<FTransformDirtyState>>(MakeUnique<FTransformDirtyState>());
        Registry.on_construct<FNeedsTransformUpdate>().connect<&OnTransformDirtied>(&State);
        return &State;
    }

    FTransformDirtyGate* EnsureTransformDirtyGate(FEntityRegistry& Registry)
    {
        return EnsureTransformDirtyState(Registry);
    }

    bool IsEntityTransformFlat(FEntityRegistry& Registry, entt::entity Entity)
    {
        return !Registry.all_of<FRelationshipComponent>(Entity);
    }

    void PublishFlatMove(FTransformDirtyGate* Gate, entt::entity Entity, bool bPublish, bool bQueueBody)
    {
        if (Gate == nullptr)
        {
            return;
        }

        // Single, non-virtual base: the gate IS the state, viewed through the part the header can see.
        FTransformDirtyState* State = static_cast<FTransformDirtyState*>(Gate);

        if (bQueueBody)
        {
            // Same per-slot token fast path as QueueDirtyTransform; the implicit enqueue is the always-safe
            // fallback for an unexpected slot or a scheduler that is not up yet.
            const uint32 Slot = State->BodyTokens.empty() ? ~0u : Jobs::GetWorkerIndex();
            if (Slot < State->BodyTokens.size())
            {
                State->DirtyBodies.enqueue(State->BodyTokens[Slot], Entity);
            }
            else
            {
                State->DirtyBodies.enqueue(Entity);
            }
        }

        if (bPublish)
        {
            State->PublishMoved(Entity);
        }
    }

    void QueueDirtyTransform(FTransformDirtyGate* Gate, entt::entity Entity, bool bQueueTransform, bool bQueueBody)
    {
        if (Gate == nullptr)
        {
            return;
        }

        FTransformDirtyState* State = static_cast<FTransformDirtyState*>(Gate);

        // Fast path: this thread's slot token (no implicit-producer hash lookup). The slot guard skips
        // GetWorkerIndex() when tokens are unset (scheduler not up), and the implicit enqueue below is the
        // always-safe fallback for any unexpected slot -- mixing explicit/implicit on one queue is allowed.
        if (!State->TransformTokens.empty())
        {
            const uint32 Slot = Jobs::GetWorkerIndex();
            if (Slot < State->TransformTokens.size())
            {
                if (bQueueTransform)
                {
                    State->DirtyTransforms.enqueue(State->TransformTokens[Slot], Entity);
                }
                if (bQueueBody)   // only bodied entities need the physics re-sync; skip the queue + drain otherwise
                {
                    State->DirtyBodies.enqueue(State->BodyTokens[Slot], Entity);
                }
                State->bAnyDirty.store(true, std::memory_order_relaxed);
                return;
            }
        }

        if (bQueueTransform)
        {
            State->DirtyTransforms.enqueue(Entity);
        }
        if (bQueueBody)
        {
            State->DirtyBodies.enqueue(Entity);
        }
        State->bAnyDirty.store(true, std::memory_order_relaxed);
    }

    void FlushDirtyPhysicsBodies(FEntityRegistry& Registry)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.ctx().find<TUniquePtr<FTransformDirtyState>>();
        FTransformDirtyState* State = Holder ? Holder->get() : nullptr;
        if (State == nullptr)
        {
            return;   // no dirty state yet -> no setter has moved anything, so no body to re-sync
        }

        // Drain the lock-free queue of setter-moved entities and tag the bodied ones for the physics sync.
        // Single-threaded (the physics boundary); the emplace is the deferred, batched MarkPhysicsBodyDirtyIfBodied.
        entt::entity Batch[128];
        std::size_t Count;
        while ((Count = State->DirtyBodies.try_dequeue_bulk(Batch, 128)) != 0)
        {
            for (std::size_t i = 0; i < Count; ++i)
            {
                const entt::entity E = Batch[i];
                if (!Registry.valid(E))
                {
                    continue;
                }

                // Release the enqueue guard as the entry is consumed, so a setter after this drain re-queues.
                if (STransformComponent* Transform = Registry.try_get<STransformComponent>(E))
                {
                    Transform->bBodyDirtyQueued = false;
                }

                if (Registry.any_of<SRigidBodyComponent, SCharacterPhysicsComponent>(E))
                {
                    Registry.emplace_or_replace<FNeedsPhysicsBodyUpdate>(E);
                }
            }
        }
    }

    // Recompute world = parentWorld * local for every descendant of Root. Walks the child links via the
    // cached relationship storage (no per-node try_get through the registry's type map).
    template<typename TTransformStorage, typename TRelStorage>
    static void PropagateTransformsToDescendants(TTransformStorage& TransformStorage, TRelStorage& RelStorage, entt::entity Root, bool bClearDirty,
                                                 FTransformDirtyState* PublishState = nullptr)
    {
        TFixedVector<entt::entity, 64> Stack;
        Stack.push_back(Root);

        while (!Stack.empty())
        {
            const entt::entity Parent = Stack.back();
            Stack.pop_back();

            if (!RelStorage.contains(Parent))
            {
                continue;
            }

            const FTransform ParentWorld = TransformStorage.get(Parent).GetWorldTransformCached();
            entt::entity Child = RelStorage.get(Parent).First;
            while (Child != entt::null)
            {
                const entt::entity Next = RelStorage.contains(Child) ? RelStorage.get(Child).Next : entt::null;

                STransformComponent& ChildTransform = TransformStorage.get(Child);
                ChildTransform.WorldTransform = ParentWorld * ChildTransform.LocalTransform;

                if (bClearDirty)
                {
                    ChildTransform.bWorldDirty = false;
                }

                // A descendant moved because its ancestor did; its own bWorldDirty was never set, so
                // this is the only place a downstream cache can learn about it.
                if (PublishState != nullptr)
                {
                    PublishState->PublishMoved(Child);
                }

                Stack.push_back(Child);
                Child = Next;
            }
        }
    }

    bool AnyTransformsDirty(FEntityRegistry& Registry)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.ctx().find<TUniquePtr<FTransformDirtyState>>();
        FTransformDirtyState* State = Holder ? Holder->get() : nullptr;
        return State != nullptr && State->bAnyDirty.load(std::memory_order_acquire);
    }

    void ResolveTransformChain(FEntityRegistry& Registry, entt::entity Entity)
    {
        FTransformDirtyState& DirtyState = *EnsureTransformDirtyState(Registry);
        if (!DirtyState.bAnyDirty.load(std::memory_order_acquire))
        {
            return;
        }

        // Serialize against the boundary / physics-thread resolvers so a shared ancestor's WorldTransform is
        // never written concurrently. Fiber-aware: parks the fiber instead of spinning a core if contended.
        FFiberScopeLock ResolveLock(DirtyState.ResolveGuard);

        TFixedVector<entt::entity, 64> AncestorChain;
        int32 TopmostDirtyIndex = -1;

        entt::entity Current = Entity;
        auto&& RelStorage = Registry.storage<FRelationshipComponent>();
        auto&& XFormStorage = Registry.storage<STransformComponent>();
        
        while (Current != entt::null)
        {
            if (XFormStorage.contains(Current) && XFormStorage.get(Current).bWorldDirty)
            {
                TopmostDirtyIndex = (int32)AncestorChain.size();
            }

            AncestorChain.push_back(Current);

            if (!Registry.all_of<FRelationshipComponent>(Current))
            {
                break;
            }

            entt::entity Parent = RelStorage.get(Current).Parent;
            if (Parent == entt::null || !Registry.valid(Parent))
            {
                break;
            }

            Current = Parent;
        }

        if (TopmostDirtyIndex < 0)
        {
            return;
        }

        for (int32 i = TopmostDirtyIndex; i >= 0; --i)
        {
            entt::entity Ancestor = AncestorChain[i];
            auto& Transform = XFormStorage.get(Ancestor);

            FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Ancestor);
            if (Rel && Rel->Parent != entt::null && Registry.valid(Rel->Parent))
            {
                const FTransform& ParentWorld = XFormStorage.get(Rel->Parent).GetWorldTransformCached();
                Transform.WorldTransform = ParentWorld * Transform.LocalTransform;
            }
            else
            {
                Transform.WorldTransform = Transform.LocalTransform;
            }

            DirtyState.PublishMoved(Ancestor);
        }

        // Propagate to the full subtree. Compute-only (no flag clearing), so no lock needed.
        PropagateTransformsToDescendants(XFormStorage, RelStorage, AncestorChain[TopmostDirtyIndex], /*bClearDirty*/ false, &DirtyState);
    }

    void SetPublishMovedTransforms(FEntityRegistry& Registry, bool bEnable)
    {
        EnsureTransformDirtyState(Registry)->bPublishMoved.store(bEnable, std::memory_order_release);
    }

    bool DrainMovedTransforms(FEntityRegistry& Registry, TVector<entt::entity>& Out)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.ctx().find<TUniquePtr<FTransformDirtyState>>();
        FTransformDirtyState* State = Holder ? Holder->get() : nullptr;
        if (State == nullptr)
        {
            return false;
        }

        const SIZE_T Before = Out.size();

        entt::entity Batch[256];
        std::size_t Count;
        while ((Count = State->MovedTransforms.try_dequeue_bulk(Batch, 256)) != 0)
        {
            Out.insert(Out.end(), Batch, Batch + Count);
        }

        // Opens the next publish window: a flat setter compares its stamp against this, so bumping it here
        // is what lets every entity publish once more. No clearing pass over the drained entities needed.
        State->PublishEpoch.fetch_add(1, std::memory_order_relaxed);

        return Out.size() != Before;
    }

    void ResolveAllDirtyTransforms(FEntityRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        // Writes WorldTransform across the registry and walks parent chains -> a calling system must declare it.
        ValidateSystemAccess(static_cast<uint32>(entt::type_hash<STransformComponent>::value()), true, "Write<STransformComponent>");
        ValidateSystemAccess(static_cast<uint32>(entt::type_hash<FRelationshipComponent>::value()), false, "Read<FRelationshipComponent>");

        FTransformDirtyState& DirtyState = *EnsureTransformDirtyState(Registry);
        FFiberScopeLock ResolveLock(DirtyState.ResolveGuard);   // one resolver writes WorldTransform at a time

        auto& TransformStorage = Registry.storage<STransformComponent>();
        auto& RelStorage       = Registry.storage<FRelationshipComponent>();   // cached: avoids per-entity try_get

        // Fold any external FNeedsTransformUpdate tags (editor/net/prefab/serialize, or tags emplaced before
        // the on_construct hook connected) into the dirty queue. bWorldDirty dedups vs already-queued ones.
        for (entt::entity Tagged : Registry.view<FNeedsTransformUpdate>())
        {
            if (TransformStorage.contains(Tagged))
            {
                STransformComponent& Xf = TransformStorage.get(Tagged);
                if (!Xf.bWorldDirty)
                {
                    Xf.bWorldDirty = true;
                    DirtyState.DirtyTransforms.enqueue(Tagged);
                    DirtyState.bAnyDirty.store(true, std::memory_order_relaxed);  // ensure the drain runs
                }
            }
        }
        Registry.clear<FNeedsTransformUpdate>();

        if (!DirtyState.bAnyDirty.load(std::memory_order_acquire))
        {
            return;
        }
        
        // Bulk-dequeue the raw dirty ids (serial but cheap -- just copies uint32s, no per-entity registry
        // work). The expensive per-entity filter + flat resolve then runs in parallel, so the whole drain
        // scales across workers instead of bottlenecking one thread (which it did before).
        TVector<entt::entity>& Raw = DirtyState.DrainScratch;
        Raw.clear();
        {
            entt::entity Batch[256];
            std::size_t Count;
            while ((Count = DirtyState.DirtyTransforms.try_dequeue_bulk(Batch, 256)) != 0)
            {
                Raw.insert(Raw.end(), Batch, Batch + Count);
            }
        }

        DirtyState.bAnyDirty.store(false, std::memory_order_release);

        if (Raw.empty())
        {
            return;
        }

        for (TVector<entt::entity>& Slot : DirtyState.HierBySlot)
        {
            Slot.clear();
        }
        
        auto Filter = [&](uint32 Index)
        {
            const entt::entity E = Raw[Index];
            if (!TransformStorage.contains(E))
            {
                return;
            }
            STransformComponent& T = TransformStorage.get(E);
            if (!T.bWorldDirty)
            {
                return;
            }
            if (RelStorage.contains(E))
            {
                uint32 Slot = Jobs::GetWorkerIndex();
                if (Slot >= DirtyState.HierBySlot.size()) { Slot = 0; }
                DirtyState.HierBySlot[Slot].push_back(E);
                return;
            }
            T.WorldTransform = T.LocalTransform;
            T.bWorldDirty    = false;
            DirtyState.PublishMoved(E);
        };

        if (Raw.size() > 1000)
        {
            Task::ParallelFor((uint32)Raw.size(), Filter);
        }
        else
        {
            for (uint32 i = 0; i < (uint32)Raw.size(); ++i)
            {
                Filter(i);
            }
        }

        // Gather the hierarchical entities from the per-slot buffers: one reserve, one memcpy per slot.
        TVector<entt::entity>& HierEntities = DirtyState.HierScratch;
        HierEntities.clear();
        {
            size_t Total = 0;
            for (const TVector<entt::entity>& Slot : DirtyState.HierBySlot)
            {
                Total += Slot.size();
            }

            HierEntities.reserve(Total);
            for (const TVector<entt::entity>& Slot : DirtyState.HierBySlot)
            {
                HierEntities.insert(HierEntities.end(), Slot.begin(), Slot.end());
            }
        }

        if (HierEntities.empty())
        {
            return;
        }

        auto ResolveHier = [&](uint32 Index)
        {
            const entt::entity DirtyEntity = HierEntities[Index];
            STransformComponent& DirtyTransform = TransformStorage.get(DirtyEntity);

            const FRelationshipComponent& Rel = RelStorage.get(DirtyEntity);
            const bool bHasParent = Rel.Parent != entt::null && Registry.valid(Rel.Parent) && TransformStorage.contains(Rel.Parent);

            if (bHasParent && TransformStorage.get(Rel.Parent).bWorldDirty)
            {
                return;
            }

            if (bHasParent)
            {
                const FTransform& ParentWorld = TransformStorage.get(Rel.Parent).GetWorldTransformCached();
                DirtyTransform.WorldTransform = ParentWorld * DirtyTransform.LocalTransform;
            }
            else
            {
                DirtyTransform.WorldTransform = DirtyTransform.LocalTransform;
            }

            DirtyState.PublishMoved(DirtyEntity);
            PropagateTransformsToDescendants(TransformStorage, RelStorage, DirtyEntity, /*bClearDirty*/ false, &DirtyState);
        };

        if (HierEntities.size() > 1000)
        {
            Task::ParallelFor((uint32)HierEntities.size(), ResolveHier);
        }
        else
        {
            for (uint32 i = 0; i < (uint32)HierEntities.size(); ++i)
            {
                ResolveHier(i);
            }
        }

        for (entt::entity Resolved : HierEntities)
        {
            if (TransformStorage.contains(Resolved))
            {
                TransformStorage.get(Resolved).bWorldDirty = false;
            }
        }
    }

    // --- Entity transform accessors ---

    FQuat GetEntityRotation(FEntityRegistry& Registry, entt::entity Entity)
    {
        auto* Transform = Registry.try_get<STransformComponent>(Entity);
        return Transform ? Transform->GetWorldRotation() : FQuat{};
    }

    FVector3 GetEntityScale(FEntityRegistry& Registry, entt::entity Entity)
    {
        auto* Transform = Registry.try_get<STransformComponent>(Entity);
        return Transform ? Transform->GetWorldScale() : FVector3{};
    }

    void SetEntityScale(FEntityRegistry& Registry, entt::entity Entity, const FVector3& Scale)
    {
        if (auto* Transform = Registry.try_get<STransformComponent>(Entity))
        {
            Transform->SetScale(Scale);
        }
    }

    namespace
    {
        // Remap one stored entity-handle id (uint32 of an entt::entity) in place: through Map if
        // present, else cleared to null when bClearUnmapped. entt::null is left as-is.
        void RemapEntityHandle(uint32& Value, const THashMap<entt::entity, entt::entity>& Map, bool bClearUnmapped)
        {
            const entt::entity Stored = static_cast<entt::entity>(Value);
            if (Stored == entt::null)
            {
                return;
            }

            auto It = Map.find(Stored);
            if (It != Map.end())
            {
                Value = static_cast<uint32>(entt::to_integral(It->second));
            }
            else if (bClearUnmapped)
            {
                Value = static_cast<uint32>(entt::to_integral(static_cast<entt::entity>(entt::null)));
            }
        }

        // Walk one struct's reflected properties: remap uint32 "Entity"-tagged handles through Map,
        // recurse into nested struct fields, and walk arrays of handles or of structs.
        void RemapEntityRefsInStruct(CStruct* Struct, void* Data, const THashMap<entt::entity, entt::entity>& Map, bool bClearUnmapped)
        {
            for (CStruct* Cur = Struct; Cur != nullptr; Cur = Cur->GetSuperStruct())
            {
                for (FProperty* Property = Cur->LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
                {
                    if (Property->IsA(EPropertyTypeFlags::UInt32) && Property->HasMetadata("Entity"))
                    {
                        uint32 Value = 0;
                        Property->GetValue(Data, &Value);
                        RemapEntityHandle(Value, Map, bClearUnmapped);
                        Property->SetValue(Data, Value);
                    }
                    else if (Property->IsA(EPropertyTypeFlags::Struct))
                    {
                        if (CStruct* Inner = static_cast<FStructProperty*>(Property)->GetStruct())
                        {
                            RemapEntityRefsInStruct(Inner, Property->GetValuePtr<void>(Data), Map, bClearUnmapped);
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

                        if (Inner->IsA(EPropertyTypeFlags::UInt32) && Property->HasMetadata("Entity"))
                        {
                            ArrayProperty->ForEach<uint32>(ArrayPtr, [&](uint32* Elem, SIZE_T)
                            {
                                RemapEntityHandle(*Elem, Map, bClearUnmapped);
                            });
                        }
                        else if (Inner->IsA(EPropertyTypeFlags::Struct))
                        {
                            if (CStruct* ElemStruct = static_cast<FStructProperty*>(Inner)->GetStruct())
                            {
                                ArrayProperty->ForEach(ArrayPtr, [&](void* Elem, SIZE_T)
                                {
                                    RemapEntityRefsInStruct(ElemStruct, Elem, Map, bClearUnmapped);
                                });
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Reflection / queries + misc ---

    void RemapEntityReferences(FEntityRegistry& Registry, entt::entity Entity, const THashMap<entt::entity, entt::entity>& Map, bool bClearUnmapped)
    {
        using namespace entt::literals;

        for (auto&& [ID, Storage] : Registry.storage())
        {
            if (!Storage.contains(Entity))
            {
                continue;
            }

            entt::meta_type MetaType = entt::resolve(Storage.info());
            if (!MetaType)
            {
                continue;
            }

            entt::meta_any Result = InvokeMetaFunc(MetaType, "static_struct"_hs);
            if (!Result)
            {
                continue;
            }

            if (CStruct* Struct = Result.cast<CStruct*>())
            {
                RemapEntityRefsInStruct(Struct, Storage.value(Entity), Map, bClearUnmapped);
            }
        }
    }

    void SetEntityWorldTransform(FEntityRegistry& Registry, entt::entity Entity, const FTransform& WorldTransform)
    {
        // Writes the entity's local transform and reads the parent's world matrix via FRelationshipComponent.
        ValidateSystemAccess(static_cast<uint32>(entt::type_hash<STransformComponent>::value()), true, "Write<STransformComponent>");
        ValidateSystemAccess(static_cast<uint32>(entt::type_hash<FRelationshipComponent>::value()), false, "Read<FRelationshipComponent>");

        STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        FMatrix4 ParentWorldMatrix(1.0f);
        if (const FRelationshipComponent* Relationship = Registry.try_get<FRelationshipComponent>(Entity))
        {
            if (Relationship->Parent != entt::null)
            {
                ParentWorldMatrix = Registry.get<STransformComponent>(Relationship->Parent).GetWorldMatrix();
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
        Transform->SetLocalTransform(NewLocal);
    }

    void DestroyEntity(FEntityRegistry& Registry, entt::entity Entity)
    {
        Registry.destroy(Entity);
    }

    entt::id_type GetTypeID(FStringView Name)
    {
        return entt::hashed_string(Name.data());
    }

    entt::id_type GetTypeID(const CStruct* Type)
    {
        return entt::hashed_string(Type->GetName().c_str());
    }

    void SetEntityBodyType(FEntityRegistry& Registry, entt::entity Entity)
    {
        Registry.emplace_or_replace<FNeedsPhysicsBodyUpdate>(Entity);
    }

    // Tag a body for the physics sync to reposition it. Single-threaded path (bodies aren't mass-moved).
    void MarkPhysicsBodyDirtyIfBodied(FEntityRegistry& Registry, entt::entity Entity)
    {
        if (Registry.any_of<SRigidBodyComponent, SCharacterPhysicsComponent>(Entity))
        {
            Registry.emplace_or_replace<FNeedsPhysicsBodyUpdate>(Entity);
        }
    }

    // External (non-setter) dirtying. The tag flows through OnTransformDirtied to set the flag. Single-threaded.
    void MarkTransformDirty(FEntityRegistry& Registry, entt::entity Entity)
    {
        Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
        MarkPhysicsBodyDirtyIfBodied(Registry, Entity);
    }

    void MarkTransformDirtyNoBody(FEntityRegistry& Registry, entt::entity Entity)
    {
        FTransformDirtyState* State = EnsureTransformDirtyState(Registry);

        if (STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
        {
            if (!Transform->bWorldDirty)
            {
                Transform->bWorldDirty = true;
                State->DirtyTransforms.enqueue(Entity);
            }
        }
        State->bAnyDirty.store(true, std::memory_order_release);
    }
}
