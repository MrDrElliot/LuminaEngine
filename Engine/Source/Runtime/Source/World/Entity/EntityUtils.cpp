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

    bool SerializeEntity(FArchive& RESTRICT Ar, ECS::FRegistry& RESTRICT Registry, ECS::FEntity& RESTRICT Entity)
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

            bool bHasRelationship = false;
            Ar << bHasRelationship;

            if (bHasRelationship)
            {
                FRelationshipComponent& RelationshipComponent = Registry.EmplaceOrReplace<FRelationshipComponent>(Entity);
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

        FRelationshipComponent* RelationshipComponent = Registry.TryGet<FRelationshipComponent>(Entity);
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

            Entry.Struct->SerializeTaggedProperties(Ar, Entry.Set->GetRaw(Entity));

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
        if (Ar.IsWriting())
        {
            Registry.Compact();

            // ONCE for the whole walk; see FComponentTypeCache for what that saves per entity.
            FComponentTypeCache TypeCache;
            TypeCache.Build(Registry);

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

            for (int32 i = 0; i < NumEntitiesSerialized; ++i)
            {
                int64 EntitySaveSize = 0;
                Ar << EntitySaveSize;

                int64 PreEntityPos = Ar.Tell();

                ECS::FEntity NewEntity = ECS::NullEntity;
                bool bSuccess = ECS::Utils::SerializeEntity(Ar, Registry, NewEntity);

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
        }

        return !Ar.HasError();
    }

    bool EntityHasTag(const FName& Tag, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return Registry.NamedStorage<STagComponent>(Tag).Contains(Entity);
    }
    
    // --- Hierarchy ---

    void AddToParent(ECS::FRegistry& Registry, ECS::FEntity Child, ECS::FEntity Parent)
    {
        FRelationshipComponent& ChildRelationship = Registry.GetOrEmplace<FRelationshipComponent>(Child);
        FRelationshipComponent& ParentRelationship = Registry.GetOrEmplace<FRelationshipComponent>(Parent);

        ChildRelationship.Parent = Parent;

        ChildRelationship.Prev = ECS::NullEntity;
        ChildRelationship.Next = ParentRelationship.First;

        if (ParentRelationship.First != ECS::NullEntity)
        {
            FRelationshipComponent& OldFirstRelationship = Registry.Get<FRelationshipComponent>(ParentRelationship.First);
            OldFirstRelationship.Prev = Child;
        }

        ParentRelationship.First = Child;
        ParentRelationship.Children++;
    }
    
    void ReparentEntity(ECS::FRegistry& Registry, ECS::FEntity Child, ECS::FEntity Parent, bool bPreserveWorld)
    {
        // Self-parent or circular hierarchy causes an infinite loop in ForEachChild.
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

        // Always guarded, since a cycle here infinite-loops ForEachChild even in shipping.
        if (Parent != ECS::NullEntity && IsDescendantOf(Registry, Parent, Child))
        {
            LOG_ERROR("Cannot create circular hierarchy - parent is a descendant of child!");
            return;
        }

        FRelationshipComponent& ChildRelationship = Registry.GetOrEmplace<FRelationshipComponent>(Child);
        STransformComponent& ChildTransform = Registry.Get<STransformComponent>(Child);

        if (ChildRelationship.Parent == Parent)
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
            AddToParent(Registry, Child, Parent);
        }
        else
        {
            ChildRelationship.Parent = ECS::NullEntity;
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

        TVector<ECS::FEntity> ToDestroy;
        CollectDescendants(Registry, Entity, ToDestroy);

        // Detach first so the parent stops pointing at freed entities, then destroy the subtree.
        if (Registry.HasAny<FRelationshipComponent>(Entity))
        {
            RemoveFromParent(Registry, Entity);
        }

        ToDestroy.push_back(Entity);

        for (ECS::FEntity E : ToDestroy)
        {
            if (Registry.IsValid(E))
            {
                Registry.Destroy(E);
            }
        }
    }

    void DetachImmediateChildren(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        TVector<ECS::FEntity> ToDestroy;
        CollectChildren(Registry, Entity, ToDestroy);
        
        for (auto It = ToDestroy.rbegin(); It != ToDestroy.rend(); ++It)
        {
            if (Registry.IsValid(*It))
            {
                RemoveFromParent(Registry, *It);
            }
        }
    }

    void RemoveFromParent(ECS::FRegistry& Registry, ECS::FEntity Child)
    {
        FRelationshipComponent* ChildRelationship = Registry.TryGet<FRelationshipComponent>(Child);
        if (!ChildRelationship || ChildRelationship->Parent == ECS::NullEntity)
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

        ECS::FEntity OldParent = ChildRelationship->Parent;
        FRelationshipComponent* ParentRelationship = Registry.TryGet<FRelationshipComponent>(OldParent);

        if (!ParentRelationship)
        {
            return;
        }

        ParentRelationship->Children--;

        if (ChildRelationship->Prev != ECS::NullEntity)
        {
            FRelationshipComponent& PrevRelationship = Registry.Get<FRelationshipComponent>(ChildRelationship->Prev);
            PrevRelationship.Next = ChildRelationship->Next;
        }
        else
        {
            ParentRelationship->First = ChildRelationship->Next;
        }

        if (ChildRelationship->Next != ECS::NullEntity)
        {
            FRelationshipComponent& NextRelationship = Registry.Get<FRelationshipComponent>(ChildRelationship->Next);
            NextRelationship.Prev = ChildRelationship->Prev;
        }

        ChildRelationship->Parent = ECS::NullEntity;
        ChildRelationship->Prev = ECS::NullEntity;
        ChildRelationship->Next = ECS::NullEntity;

        // Bake the snapshot back as local so the detached entity keeps its world placement.
        if (bHasTransform)
        {
            SetEntityWorldTransform(Registry, Child, WorldSnapshot);
        }
    }

    bool IsDescendantOf(ECS::FRegistry& Registry, ECS::FEntity Potential, ECS::FEntity Ancestor)
    {
        if (Potential == ECS::NullEntity || Ancestor == ECS::NullEntity)
        {
            return false;
        }

        ECS::FEntity Current = Potential;
        while (Current != ECS::NullEntity)
        {
            FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Current);
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

    bool IsChild(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Entity);
        return Relationship ? Relationship->Parent != ECS::NullEntity : false;
    }

    bool IsParent(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return GetChildCount(Registry, Entity) != 0;
    }

    ECS::FEntity GetRootEntity(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        ECS::FEntity Current = Entity;
        while (Current != ECS::NullEntity)
        {
            FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Current);
            if (!Relationship || Relationship->Parent == ECS::NullEntity)
            {
                break;
            }

            Current = Relationship->Parent;
        }

        return Current;
    }

    size_t GetChildCount(ECS::FRegistry& Registry, ECS::FEntity Parent)
    {
        FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Parent);
        return Relationship ? Relationship->Children : 0;
    }

    void CollectDescendants(ECS::FRegistry& Registry, ECS::FEntity Entity, TVector<ECS::FEntity>& OutDescendants)
    {
        OutDescendants.push_back(Entity);

        FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Entity);
        if (!Relationship || Relationship->First == ECS::NullEntity)
        {
            return;
        }

        ECS::FEntity Current = Relationship->First;
        while (Current != ECS::NullEntity)
        {
            CollectDescendants(Registry, Current, OutDescendants);

            FRelationshipComponent* CurrentRelationship = Registry.TryGet<FRelationshipComponent>(Current);
            Current = CurrentRelationship ? CurrentRelationship->Next : ECS::NullEntity;
        }
    }

    void CollectChildren(ECS::FRegistry& Registry, ECS::FEntity Entity, TVector<ECS::FEntity>& OutChildren)
    {
        FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Entity);
        if (!Relationship || Relationship->First == ECS::NullEntity)
        {
            return;
        }

        ECS::FEntity Current = Relationship->First;
        while (Current != ECS::NullEntity)
        {
            OutChildren.push_back(Current);

            FRelationshipComponent* CurrentRelationship = Registry.TryGet<FRelationshipComponent>(Current);
            Current = CurrentRelationship ? CurrentRelationship->Next : ECS::NullEntity;
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
        TVector<ECS::FEntity>          DrainScratch;
        TVector<TVector<ECS::FEntity>> HierBySlot;

        // Merged HierBySlot, kept as a member so the resolve reuses one allocation across frames.
        TVector<ECS::FEntity>          HierScratch;

        // Published moved-transform channel, off until a consumer opts in; duplicates are fine.
        std::atomic<bool> bPublishMoved{ false };
        FDirtyQueue       MovedTransforms;

        FORCEINLINE void PublishMoved(ECS::FEntity Entity)
        {
            if (bPublishMoved.load(std::memory_order_relaxed))
            {
                MovedTransforms.Enqueue(Entity);
            }
        }

        FTransformDirtyState()
        {
            const uint32 Slots = Jobs::IsInitialized() ? Jobs::GetNumThreadSlots() : 1;
            HierBySlot.resize(Slots);

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

    FTransformDirtyState* EnsureTransformDirtyState(ECS::FRegistry& Registry)
    {
        if (TUniquePtr<FTransformDirtyState>* Holder = Registry.Ctx().Find<TUniquePtr<FTransformDirtyState>>())
        {
            return Holder->get();
        }

        FTransformDirtyState& State = *Registry.Ctx().Emplace<TUniquePtr<FTransformDirtyState>>(MakeUnique<FTransformDirtyState>());
        Registry.GetSignals<FNeedsTransformUpdate>().OnConstruct.Connect<&OnTransformDirtied>(&State);
        return &State;
    }

    FTransformDirtyGate* EnsureTransformDirtyGate(ECS::FRegistry& Registry)
    {
        return EnsureTransformDirtyState(Registry);
    }

    bool IsEntityTransformFlat(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return !Registry.HasAll<FRelationshipComponent>(Entity);
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

    // Recompute world from parentWorld and local for every descendant via cached relationship storage.
    template<typename TTransformStorage, typename TRelStorage>
    static void PropagateTransformsToDescendants(TTransformStorage& TransformStorage, TRelStorage& RelStorage, ECS::FEntity Root, bool bClearDirty,
                                                 FTransformDirtyState* PublishState = nullptr)
    {
        TFixedVector<ECS::FEntity, 64> Stack;
        Stack.push_back(Root);

        while (!Stack.empty())
        {
            const ECS::FEntity Parent = Stack.back();
            Stack.pop_back();

            if (!RelStorage.Contains(Parent))
            {
                continue;
            }

            const FTransform ParentWorld = TransformStorage.Get(Parent).GetWorldTransformCached();
            ECS::FEntity Child = RelStorage.Get(Parent).First;
            while (Child != ECS::NullEntity)
            {
                const ECS::FEntity Next = RelStorage.Contains(Child) ? RelStorage.Get(Child).Next : ECS::NullEntity;

                STransformComponent& ChildTransform = TransformStorage.Get(Child);
                ChildTransform.WorldTransform = ParentWorld * ChildTransform.LocalTransform;

                if (bClearDirty)
                {
                    ChildTransform.bWorldDirty = false;
                }

                // A descendant moved because its ancestor did, so its own bWorldDirty was never set.
                if (PublishState != nullptr)
                {
                    PublishState->PublishMoved(Child);
                }

                Stack.push_back(Child);
                Child = Next;
            }
        }
    }

    bool AnyTransformsDirty(ECS::FRegistry& Registry)
    {
        TUniquePtr<FTransformDirtyState>* Holder = Registry.Ctx().Find<TUniquePtr<FTransformDirtyState>>();
        FTransformDirtyState* State = Holder ? Holder->get() : nullptr;
        return State != nullptr && State->bAnyDirty.load(std::memory_order_acquire);
    }

    void ResolveTransformChain(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        FTransformDirtyState& DirtyState = *EnsureTransformDirtyState(Registry);
        if (!DirtyState.bAnyDirty.load(std::memory_order_acquire))
        {
            return;
        }

        // Serialized against the other resolvers so a shared ancestor is never written concurrently.
        FFiberScopeLock ResolveLock(DirtyState.ResolveGuard);

        TFixedVector<ECS::FEntity, 64> AncestorChain;
        int32 TopmostDirtyIndex = -1;

        ECS::FEntity Current = Entity;
        auto RelStorage = Registry.GetStorage<FRelationshipComponent>();
        auto XFormStorage = Registry.GetStorage<STransformComponent>();
        
        while (Current != ECS::NullEntity)
        {
            if (XFormStorage.Contains(Current) && XFormStorage.Get(Current).bWorldDirty)
            {
                TopmostDirtyIndex = (int32)AncestorChain.size();
            }

            AncestorChain.push_back(Current);

            if (!Registry.HasAll<FRelationshipComponent>(Current))
            {
                break;
            }

            ECS::FEntity Parent = RelStorage.Get(Current).Parent;
            if (Parent == ECS::NullEntity || !Registry.IsValid(Parent))
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
            ECS::FEntity Ancestor = AncestorChain[i];
            auto& Transform = XFormStorage.Get(Ancestor);

            FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Ancestor);
            if (Rel && Rel->Parent != ECS::NullEntity && Registry.IsValid(Rel->Parent))
            {
                const FTransform& ParentWorld = XFormStorage.Get(Rel->Parent).GetWorldTransformCached();
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

    void SetPublishMovedTransforms(ECS::FRegistry& Registry, bool bEnable)
    {
        EnsureTransformDirtyState(Registry)->bPublishMoved.store(bEnable, std::memory_order_release);
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

        // Writes WorldTransform across the registry and walks parent chains -> a calling system must declare it.
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<STransformComponent>()), true, "Write<STransformComponent>");
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<FRelationshipComponent>()), false, "Read<FRelationshipComponent>");

        FTransformDirtyState& DirtyState = *EnsureTransformDirtyState(Registry);
        FFiberScopeLock ResolveLock(DirtyState.ResolveGuard);   // one resolver writes WorldTransform at a time

        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto RelStorage       = Registry.GetStorage<FRelationshipComponent>();   // cached, avoids per-entity try_get

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

        for (TVector<ECS::FEntity>& Slot : DirtyState.HierBySlot)
        {
            Slot.clear();
        }
        
        auto Filter = [&](uint32 Index)
        {
            const ECS::FEntity E = Raw[Index];
            if (!TransformStorage.Contains(E))
            {
                return;
            }
            STransformComponent& T = TransformStorage.Get(E);
            if (!T.bWorldDirty)
            {
                return;
            }
            if (RelStorage.Contains(E))
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

        // Gather hierarchical entities from the per-slot buffers with one reserve and one memcpy each.
        TVector<ECS::FEntity>& HierEntities = DirtyState.HierScratch;
        HierEntities.clear();
        {
            size_t Total = 0;
            for (const TVector<ECS::FEntity>& Slot : DirtyState.HierBySlot)
            {
                Total += Slot.size();
            }

            HierEntities.reserve(Total);
            for (const TVector<ECS::FEntity>& Slot : DirtyState.HierBySlot)
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
            const ECS::FEntity DirtyEntity = HierEntities[Index];
            STransformComponent& DirtyTransform = TransformStorage.Get(DirtyEntity);

            const FRelationshipComponent& Rel = RelStorage.Get(DirtyEntity);
            const bool bHasParent = Rel.Parent != ECS::NullEntity && Registry.IsValid(Rel.Parent) && TransformStorage.Contains(Rel.Parent);

            if (bHasParent && TransformStorage.Get(Rel.Parent).bWorldDirty)
            {
                return;
            }

            if (bHasParent)
            {
                const FTransform& ParentWorld = TransformStorage.Get(Rel.Parent).GetWorldTransformCached();
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

        for (ECS::FEntity Resolved : HierEntities)
        {
            if (TransformStorage.Contains(Resolved))
            {
                TransformStorage.Get(Resolved).bWorldDirty = false;
            }
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

        // One pass over LinkedProperty, NOT one per super, since Link splices the super chain onto it.
        void RemapEntityRefsInStruct(CStruct* Struct, void* Data, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped)
        {
            for (FProperty* Property = Struct->LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
            {
                if (Property->IsA(EPropertyTypeFlags::UInt32) && Property->IsEntityHandle())
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

                    if (Inner->IsA(EPropertyTypeFlags::UInt32) && Property->IsEntityHandle())
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

    // --- Reflection / queries + misc ---

    void RemapEntityReferences(ECS::FRegistry& Registry, ECS::FEntity Entity, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped)
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
                RemapEntityRefsInStruct(Struct, Storage.GetRaw(Entity), Map, bClearUnmapped);
            }
        }

        // A script is held BY a component, so its reflected properties are invisible to the storage walk.
        if (SEntityScriptComponent* Scripts = Registry.TryGet<SEntityScriptComponent>(Entity))
        {
            for (const TObjectPtr<CEntityScript>& Held : Scripts->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() != nullptr)
                {
                    RemapEntityRefsInStruct(Script->GetClass(), Script, Map, bClearUnmapped);
                }
            }
        }
    }

    void SetEntityWorldTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FTransform& WorldTransform)
    {
        // Writes the entity's local transform and reads the parent's world matrix via FRelationshipComponent.
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<STransformComponent>()), true, "Write<STransformComponent>");
        ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<FRelationshipComponent>()), false, "Read<FRelationshipComponent>");

        STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        FMatrix4 ParentWorldMatrix(1.0f);
        if (const FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Entity))
        {
            if (Relationship->Parent != ECS::NullEntity)
            {
                ParentWorldMatrix = Registry.Get<STransformComponent>(Relationship->Parent).GetWorldMatrix();
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
