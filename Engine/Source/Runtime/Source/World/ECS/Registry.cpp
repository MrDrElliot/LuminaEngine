#include "RuntimePCH.h"
#include "Registry.h"

#include "Core/Assertions/Assert.h"
#include "Core/Object/Class.h"
#include "Core/Threading/Thread.h"

namespace Lumina::ECS
{
    FComponentTypeRegistry& FComponentTypeRegistry::Get()
    {
        static FComponentTypeRegistry Instance;
        return Instance;
    }

    FComponentTypeRegistry::FComponentTypeRegistry()
    {
        Types.Reserve(MaxComponentTypes);
    }

    void FComponentTypeRegistry::BindStruct(FComponentTypeID TypeID, CStruct* Struct)
    {
        if (TypeID < Types.Num())
        {
            Types[TypeID]->CachedStruct = Struct;
        }
    }

    void FComponentTypeRegistry::SetEventFanOut(FComponentTypeID TypeID, FEventFanOut FanOut)
    {
        if (TypeID < Types.Num())
        {
            Types[TypeID]->EventFanOut = FanOut;
        }
    }

    FComponentTypeID FComponentTypeRegistry::Acquire(const FComponentTypeInfo& Info)
    {
        FScopeLock Lock(TypesLock);

        if (const FComponentTypeID Existing = FindByNameLocked(Info.Name); Existing != InvalidComponentTypeID)
        {
            return Existing;
        }

        ASSERT(Types.Num() < InvalidComponentTypeID);
        ASSERT(static_cast<size_t>(Types.Num()) < MaxComponentTypes);

        TUniquePtr<FComponentTypeInfo> Stored = MakeUnique<FComponentTypeInfo>(Info);
        Stored->TypeID = static_cast<FComponentTypeID>(Types.Num());

        const FComponentTypeID TypeID = Stored->TypeID;
        Types.Add(std::move(Stored));
        return TypeID;
    }

    const FComponentTypeInfo& FComponentTypeRegistry::GetInfo(FComponentTypeID TypeID) const
    {
        ASSERT(TypeID < Types.Num());
        return *Types[TypeID];
    }

    FComponentTypeID FComponentTypeRegistry::FindByName(const FName& Name) const
    {
        FScopeLock Lock(TypesLock);
        return FindByNameLocked(Name);
    }

    FComponentTypeID FComponentTypeRegistry::FindByNameLocked(const FName& Name) const
    {
        for (const TUniquePtr<FComponentTypeInfo>& Info : Types)
        {
            if (Info->Name == Name)
            {
                return Info->TypeID;
            }
        }
        return InvalidComponentTypeID;
    }

    FComponentTypeID FComponentTypeRegistry::FindByStruct(const CStruct* Struct) const
    {
        if (Struct == nullptr)
        {
            return InvalidComponentTypeID;
        }

        FScopeLock Lock(TypesLock);
        for (const TUniquePtr<FComponentTypeInfo>& Info : Types)
        {
            if (Info->GetStruct() == Struct)
            {
                return Info->TypeID;
            }
        }
        return InvalidComponentTypeID;
    }

    int32 FComponentTypeRegistry::Num() const
    {
        FScopeLock Lock(TypesLock);
        return Types.Num();
    }

    void FComponentTypeRegistry::Relayout(FComponentTypeID TypeID, const FComponentTypeInfo& Info, FMigrateComponentValue Migrate)
    {
        FComponentTypeInfo* StoredPtr = nullptr;
        {
            FScopeLock Lock(TypesLock);
            ASSERT(TypeID < Types.Num());
            StoredPtr = &*Types[TypeID];
            const FEventFanOut FanOut = StoredPtr->EventFanOut;
            *StoredPtr = Info;
            StoredPtr->EventFanOut = FanOut;
            StoredPtr->TypeID = TypeID;
            StoredPtr->bRuntimeLayout = true;
        }
        FComponentTypeInfo& Stored = *StoredPtr;

        FScopeLock Lock(PoolLock);
        for (FSparseSet* Pool : RuntimeLayoutPools)
        {
            if (Pool->GetTypeID() == TypeID)
            {
                Pool->Relayout(Stored, Migrate);
            }
        }
    }

    void FComponentTypeRegistry::TrackPool(FSparseSet* Pool)
    {
        FScopeLock Lock(PoolLock);
        RuntimeLayoutPools.Add(Pool);
    }

    void FComponentTypeRegistry::UntrackPool(FSparseSet* Pool)
    {
        FScopeLock Lock(PoolLock);
        for (int32 Index = 0; Index < RuntimeLayoutPools.Num(); ++Index)
        {
            if (RuntimeLayoutPools[Index] == Pool)
            {
                RuntimeLayoutPools[Index] = RuntimeLayoutPools.Last();
                RuntimeLayoutPools.Pop();
                return;
            }
        }
    }



    // Slots past EntityRecords.Num() up to NextAppendIndex are handed out but not yet stored, so a reservation never reallocates the records a reader may be walking.
    struct FEntityAllocState
    {
        FMutex Mutex;
        uint32 NextAppendIndex = 0;
    };

    FEntityAllocHolder::FEntityAllocHolder() : State(MakeUnique<FEntityAllocState>()) {}
    FEntityAllocHolder::~FEntityAllocHolder() = default;

    FEntityAllocHolder::FEntityAllocHolder(FEntityAllocHolder&& Other) noexcept
        : State(Move(Other.State))
    {
        Other.State = MakeUnique<FEntityAllocState>();
    }

    FEntityAllocHolder& FEntityAllocHolder::operator = (FEntityAllocHolder&& Other) noexcept
    {
        State = Move(Other.State);
        Other.State = MakeUnique<FEntityAllocState>();
        return *this;
    }

    void FEntityAllocHolder::Swap(FEntityAllocHolder& Other) noexcept
    {
        TUniquePtr<FEntityAllocState> Mine = Move(State);
        State = Move(Other.State);
        Other.State = Move(Mine);
    }

    FRegistry::FRegistry() = default;
    FRegistry::~FRegistry() = default;

    FRegistry::FRegistry(FRegistry&& Other) noexcept = default;
    FRegistry& FRegistry::operator = (FRegistry&& Other) noexcept = default;

    uint32 FRegistry::TakeSlotLocked(uint32& OutVersion)
    {
        if (FreeEntityHead != NoFreeSlot)
        {
            const uint32 Index = FreeEntityHead;
            FEntity& Record = EntityRecords[Index];
            FreeEntityHead = Record.GetIndex();
            OutVersion = Record.GetVersion();
            Record = FEntity(NoFreeSlot, OutVersion);
            return Index;
        }

        const uint32 Index = Math::Max(AllocState->NextAppendIndex, static_cast<uint32>(EntityRecords.Num()));
        ASSERT(Index <= FEntity::MaxIndex);
        AllocState->NextAppendIndex = Index + 1u;
        OutVersion = 0u;
        return Index;
    }

    void FRegistry::EnsureRecordLocked(uint32 Index)
    {
        if (Index >= static_cast<uint32>(EntityRecords.Num()))
        {
            // The slots in between belong to reservations other threads still hold, so they stay unlinked.
            EntityRecords.SetNum(Index + 1u, FEntity(NoFreeSlot, 0u));
        }
    }

    FEntity FRegistry::Create()
    {
        FEntity Created;
        {
            TScopeLock<FMutex> Lock(AllocState->Mutex);
            uint32 Version = 0u;
            const uint32 Index = TakeSlotLocked(Version);
            EnsureRecordLocked(Index);
            Created = FEntity(Index, Version);
            EntityRecords[Index] = Created;
        }

        ++LiveEntityCount;
        EntityCreated.Broadcast(*this, Created);
        return Created;
    }

    FEntity FRegistry::CreateInFreshSlot()
    {
        FEntity Created;
        {
            TScopeLock<FMutex> Lock(AllocState->Mutex);
            const uint32 Index = Math::Max(AllocState->NextAppendIndex, static_cast<uint32>(EntityRecords.Num()));
            ASSERT(Index <= FEntity::MaxIndex);
            AllocState->NextAppendIndex = Index + 1u;
            EnsureRecordLocked(Index);
            Created = FEntity(Index, 0u);
            EntityRecords[Index] = Created;
        }

        ++LiveEntityCount;
        EntityCreated.Broadcast(*this, Created);
        return Created;
    }

    FEntity FRegistry::ReserveEntity()
    {
        TScopeLock<FMutex> Lock(AllocState->Mutex);
        uint32 Version = 0u;
        const uint32 Index = TakeSlotLocked(Version);
        return FEntity(Index, Version);
    }

    void FRegistry::MaterializeReserved(FEntity Entity)
    {
        const uint32 Index = Entity.GetIndex();
        {
            TScopeLock<FMutex> Lock(AllocState->Mutex);
            EnsureRecordLocked(Index);
            if (EntityRecords[Index].GetIndex() != NoFreeSlot)
            {
                return;
            }
            EntityRecords[Index] = Entity;
        }

        ++LiveEntityCount;
        EntityCreated.Broadcast(*this, Entity);
    }

    void FRegistry::ReleaseReserved(FEntity Entity)
    {
        const uint32 Index = Entity.GetIndex();
        TScopeLock<FMutex> Lock(AllocState->Mutex);
        EnsureRecordLocked(Index);
        if (EntityRecords[Index].GetIndex() != NoFreeSlot)
        {
            return;
        }
        EntityRecords[Index] = FEntity(FreeEntityHead, Entity.GetVersion());
        FreeEntityHead = Index;
    }

    FEntity FRegistry::Create(FEntity Hint)
    {
        if (Hint.IsNull())
        {
            return Create();
        }

        const uint32 Index = Hint.GetIndex();
        ASSERT(Index <= FEntity::MaxIndex);

        if (Index >= static_cast<uint32>(EntityRecords.Num()))
        {
            TScopeLock<FMutex> Lock(AllocState->Mutex);

            // Everything between the end and the hint becomes free, linked head-first so Create reuses it.
            const uint32 FirstNew = static_cast<uint32>(EntityRecords.Num());
            EntityRecords.SetNum(Index + 1u);

            for (uint32 Slot = FirstNew; Slot < Index; ++Slot)
            {
                EntityRecords[Slot] = FEntity(FreeEntityHead, 0);
                FreeEntityHead = Slot;
            }

            EntityRecords[Index] = Hint;
            AllocState->NextAppendIndex = Math::Max(AllocState->NextAppendIndex, Index + 1u);
            ++LiveEntityCount;
            EntityCreated.Broadcast(*this, Hint);
            return Hint;
        }

        if (EntityRecords[Index].GetIndex() == Index)
        {
            // The slot is live, so the caller gets a fresh handle instead of a collision.
            return Create();
        }

        // Unlink the slot from wherever it sits in the free chain, then stamp the requested version.
        if (FreeEntityHead == Index)
        {
            FreeEntityHead = EntityRecords[Index].GetIndex();
        }
        else
        {
            uint32 Previous = FreeEntityHead;
            while (Previous != NoFreeSlot && EntityRecords[Previous].GetIndex() != Index)
            {
                Previous = EntityRecords[Previous].GetIndex();
            }
            if (Previous != NoFreeSlot)
            {
                EntityRecords[Previous] = FEntity(EntityRecords[Index].GetIndex(), EntityRecords[Previous].GetVersion());
            }
        }

        EntityRecords[Index] = Hint;
        ++LiveEntityCount;
        EntityCreated.Broadcast(*this, Hint);
        return Hint;
    }

    void FRegistry::Destroy(FEntity Entity)
    {
        if (!IsValid(Entity) || DestroyingEntities.Contains(Entity))
        {
            return;
        }

        DestroyingEntities.Add(Entity);
        EntityDestroyed.Broadcast(*this, Entity);
        DestroyHierarchyLinks(Entity);
        DetachFromAllStorages(Entity);
        DestroyingEntities.Pop();

        const uint32 Index = Entity.GetIndex();
        {
            TScopeLock<FMutex> Lock(AllocState->Mutex);
            FEntity& Record = EntityRecords[Index];
            Record = FEntity(FreeEntityHead, Record.GetNextVersion());
            FreeEntityHead = Index;
        }
        --LiveEntityCount;
    }

    void FRegistry::DestroyHierarchyLinks(FEntity Entity)
    {
        if (!Hierarchy.IsLinked(Entity))
        {
            Hierarchy.Forget(Entity);
            return;
        }

        const FEntity Parent = Hierarchy.GetParent(Entity);
        Hierarchy.Unlink(Entity);
        if (!Parent.IsNull())
        {
            HierarchyChanged.Broadcast(*this, Parent);
        }

        if (bDestroyDescendantsWithParent)
        {
            TVector<FEntity> Subtree;
            Hierarchy.ForEachDescendant(Entity, [&](FEntity Descendant) { Subtree.Add(Descendant); });

            // Reversed, so every child goes before its parent, and unlinked first, since nobody is left to tell.
            for (int32 Index = Subtree.Num(); Index-- > 0;)
            {
                Hierarchy.Unlink(Subtree[Index]);
                Destroy(Subtree[Index]);
            }
        }
        else
        {
            Hierarchy.ForEachChild(Entity, [&](FEntity Orphan)
            {
                Hierarchy.Unlink(Orphan);
                HierarchyChanged.Broadcast(*this, Orphan);
            });
        }

        Hierarchy.Forget(Entity);
    }

    bool FRegistry::AttachChild(FEntity Child, FEntity Parent, uint32 Position)
    {
        if (Child == Parent || !IsValid(Child) || !IsValid(Parent) || Hierarchy.IsDescendantOf(Parent, Child))
        {
            return false;
        }

        const bool bSameParent = Hierarchy.GetParent(Child) == Parent;
        if (bSameParent && Position == FHierarchy::AppendPosition)
        {
            return true;
        }

        const FEntity OldParent = Hierarchy.GetParent(Child);
        Hierarchy.Unlink(Child);
        Hierarchy.Link(Child, Parent, Position);
        if (!bSameParent)
        {
            HierarchyChanged.Broadcast(*this, Child);
            HierarchyChanged.Broadcast(*this, Parent);
            if (!OldParent.IsNull())
            {
                HierarchyChanged.Broadcast(*this, OldParent);
            }
        }
        return true;
    }

    void FRegistry::DetachFromParent(FEntity Child)
    {
        const FEntity OldParent = Hierarchy.GetParent(Child);
        if (OldParent.IsNull())
        {
            return;
        }
        Hierarchy.Unlink(Child);
        HierarchyChanged.Broadcast(*this, Child);
        HierarchyChanged.Broadcast(*this, OldParent);
    }

    // Indexed and repeated, since an OnDestroy handler can create a pool or add to one already passed; the last pass is silent.
    void FRegistry::DetachFromAllStorages(FEntity Entity)
    {
        constexpr int32 MaxNotifyingPasses = 4;
        for (int32 Pass = 0; Pass <= MaxNotifyingPasses; ++Pass)
        {
            bool bDetachedAny = false;
            for (int32 Index = 0; Index < ActiveStorages.Num(); ++Index)
            {
                FSparseSet* Storage = ActiveStorages[Index];
                if (!Storage->Contains(Entity))
                {
                    continue;
                }
                if (Pass < MaxNotifyingPasses)
                {
                    Storage->Signals.OnDestroy.Broadcast(*this, Entity);
                }
                if (Storage->Contains(Entity))
                {
                    Storage->RemoveEntity(Entity);
                }
                bDetachedAny = true;
            }
            if (!bDetachedAny)
            {
                return;
            }
        }
        LOG_WARN("Entity {} kept gaining components while it was destroyed; the last ones were removed without notifying.", Entity.GetPacked());
    }

    void FRegistry::Clear()
    {
        for (FSparseSet* Storage : ActiveStorages)
        {
            Storage->ClearAll();
        }

        Hierarchy.Reset();
        EntityRecords.Reset();
        FreeEntityHead = NoFreeSlot;
        LiveEntityCount = 0;
        AllocState->NextAppendIndex = 0u;

        // An emptied registry is often refilled by a loader thread, which may claim it afresh.
        PoolTableOwner = 0;
    }

    void FRegistry::Reserve(int32 EntityCount)
    {
        EntityRecords.Reserve(EntityCount);
    }

    void FRegistry::Compact()
    {
        for (FSparseSet* Storage : ActiveStorages)
        {
            Storage->Compact();
        }
    }

    void FRegistry::Swap(FRegistry& Other) noexcept
    {
        swap(EntityRecords, Other.EntityRecords);
        swap(OwnedStorages, Other.OwnedStorages);
        swap(Groups, Other.Groups);
        swap(StoragesByTypeID, Other.StoragesByTypeID);
        swap(ActiveStorages, Other.ActiveStorages);
        swap(NamedStorages, Other.NamedStorages);
        Context.Swap(Other.Context);
        std::swap(Hierarchy, Other.Hierarchy);

        const uint32 FreeHead = FreeEntityHead;
        FreeEntityHead = Other.FreeEntityHead;
        Other.FreeEntityHead = FreeHead;

        AllocState.Swap(Other.AllocState);

        const int32 LiveCount = LiveEntityCount;
        LiveEntityCount = Other.LiveEntityCount;
        Other.LiveEntityCount = LiveCount;

        // Both pool tables now belong to whoever swapped them, rather than to whichever loader filled one.
        PoolTableOwner = 0;
        Other.PoolTableOwner = 0;
        CheckPoolTableThread();
        Other.CheckPoolTableThread();
    }

    uint64 FRegistry::MakeNamedStorageKey(FComponentTypeID TypeID, const FName& Name)
    {
        return (static_cast<uint64>(TypeID) << 32) | static_cast<uint64>(Name.Hash() & 0xFFFFFFFFull);
    }

    FSparseSet* FRegistry::FindNamedStorage(FComponentTypeID TypeID, const FName& Name) const
    {
        const auto It = NamedStorages.Find(MakeNamedStorageKey(TypeID, Name));
        return It != nullptr ? It->get() : nullptr;
    }

    FSparseSet* FRegistry::AssureNamedStorage(FComponentTypeID TypeID, const FName& Name)
    {
        const uint64 Key = MakeNamedStorageKey(TypeID, Name);

        const auto It = NamedStorages.Find(Key);
        if (It != nullptr)
        {
            return It->get();
        }

        TUniquePtr<FSparseSet> Storage = MakeUnique<FSparseSet>(FComponentTypeRegistry::Get().GetInfo(TypeID));
        FSparseSet* Raw = Storage.Get();

        CheckPoolTableThread();
        NamedStorages.TryAdd(Key, std::move(Storage));

        // Joins the walk so a destroy detaches from it too, but never the by-type-id index, which one pool owns.
        ActiveStorages.Add(Raw);
        return Raw;
    }

    FSparseSet& FRegistry::AssureStorage(FComponentTypeID TypeID)
    {
        if (TypeID >= StoragesByTypeID.Num() || StoragesByTypeID[TypeID] == nullptr)
        {
            CreateStorage(TypeID);
        }
        return *StoragesByTypeID[TypeID];
    }

    void* FRegistry::GetOrEmplaceRaw(FComponentTypeID TypeID, FEntity Entity)
    {
        FSparseSet& Storage = AssureStorage(TypeID);
        if (Storage.Contains(Entity))
        {
            return Storage.GetRawForWrite(Entity);
        }
        void* Value = Storage.EmplaceDefaultRaw(Entity);
        Storage.Signals.OnConstruct.Broadcast(*this, Entity);
        return Value;
    }

    void* FRegistry::EmplaceOrReplaceRaw(FComponentTypeID TypeID, FEntity Entity, const void* Source)
    {
        FSparseSet& Storage = AssureStorage(TypeID);
        const bool bExisted = Storage.Contains(Entity);
        void* Value = Storage.EmplaceCopyRaw(Entity, Source);
        (bExisted ? Storage.Signals.OnUpdate : Storage.Signals.OnConstruct).Broadcast(*this, Entity);
        return Value;
    }

    bool FRegistry::RemoveRaw(FComponentTypeID TypeID, FEntity Entity)
    {
        FSparseSet* Storage = FindStorage(TypeID);
        if (Storage == nullptr || !Storage->Contains(Entity))
        {
            return false;
        }
        Storage->Signals.OnDestroy.Broadcast(*this, Entity);
        return Storage->RemoveEntity(Entity);
    }

    void FRegistry::PatchRaw(FComponentTypeID TypeID, FEntity Entity)
    {
        FSparseSet* Storage = FindStorage(TypeID);
        if (Storage != nullptr && !Storage->IsTagOnly() && Storage->Contains(Entity))
        {
            Storage->MarkWritten(Storage->GetDenseIndex(Entity));
            Storage->Signals.OnUpdate.Broadcast(*this, Entity);
        }
    }

    void FRegistry::MarkChangedRaw(FComponentTypeID TypeID, FEntity Entity)
    {
        FSparseSet* Storage = FindStorage(TypeID);
        if (Storage != nullptr && Storage->Contains(Entity))
        {
            Storage->MarkWritten(Storage->GetDenseIndex(Entity));
        }
    }

    uint32 FRegistry::AdvanceChangeTick()
    {
        const uint32 Previous = FSparseSet::AdvanceChangeTick();
        ExpireEscapedPointers();
        return Previous;
    }

    void FRegistry::ExpireEscapedPointers()
    {
        for (FSparseSet* Storage : ActiveStorages)
        {
            Storage->ExpireEscapedPointers();
        }
    }

    void FRegistry::AssureStorages(TSpan<const FComponentTypeID> TypeIDs)
    {
        for (const FComponentTypeID TypeID : TypeIDs)
        {
            (void)AssureStorage(TypeID);
        }
    }

    void FRegistry::CheckPoolTableThread()
    {
#if defined(LE_DEBUG) || defined(LE_DEVELOPMENT)
        const uint64 Self = Threading::GetThreadID();
        if (PoolTableOwner == 0)
        {
            PoolTableOwner = Self;
        }
        DEBUG_ASSERT(PoolTableOwner == Self || Threading::IsMainThread(),
            "Created a component pool off the registry's thread, which races every view and lookup reading the pool table. Create it first with AssureStorages, or read through FindView.");
#endif
    }

    void FRegistry::CreateStorage(FComponentTypeID TypeID)
    {
        CheckPoolTableThread();

        if (TypeID >= StoragesByTypeID.Num())
        {
            StoragesByTypeID.SetNum(TypeID + 1u, nullptr);
        }

        TUniquePtr<FSparseSet> Storage = MakeUnique<FSparseSet>(FComponentTypeRegistry::Get().GetInfo(TypeID));

        FSparseSet* Raw = Storage.Get();
        StoragesByTypeID[TypeID] = Raw;
        ActiveStorages.Add(Raw);
        OwnedStorages.Add(std::move(Storage));
    }

    FOwningGroup* FRegistry::DeclareGroup(TSpan<const FComponentTypeID> TypeIDs)
    {
        if (TypeIDs.Num() < 2)
        {
            LOG_ERROR("Cannot declare a group of {} component types: a group owns at least two.", TypeIDs.Num());
            return nullptr;
        }

        TInlineVector<FSparseSet*, 8> Sets;
        for (const FComponentTypeID TypeID : TypeIDs)
        {
            FSparseSet* Set = &AssureStorage(TypeID);
            if (Sets.Contains(Set))
            {
                LOG_ERROR("Cannot declare a group listing {} twice.", Set->GetName());
                return nullptr;
            }
            Sets.Add(Set);
        }

        FOwningGroup* Existing = Sets[0]->OwningGroup;
        if (Existing != nullptr && Existing->Owned.Num() == Sets.Num()
            && Sets.FindByPredicate([Existing](const FSparseSet* Set) { return Set->OwningGroup != Existing; }) == nullptr)
        {
            return Existing;
        }

        for (const FSparseSet* Set : Sets)
        {
            if (Set->OwningGroup != nullptr)
            {
                LOG_ERROR("Cannot declare a group owning {}: another group already owns it, and a component type can be owned by one group only.", Set->GetName());
                return nullptr;
            }
            if (Set->bInPlaceDelete)
            {
                LOG_ERROR("Cannot declare a group owning {}: an in-place-delete pool leaves holes, which a packed group cannot hold.", Set->GetName());
                return nullptr;
            }
        }

        FOwningGroup* Group = Groups.EmplaceGetRef(MakeUnique<FOwningGroup>()).Get();
        Group->Owned = Sets;

        FSparseSet* Smallest = Sets[0];
        for (FSparseSet* Set : Sets)
        {
            Set->OwningGroup = Group;
            if (Set->Num() < Smallest->Num())
            {
                Smallest = Set;
            }
        }

        for (int32 Index = 0; Index < Smallest->GetDenseSize(); ++Index)
        {
            const FEntity Entity = Smallest->Dense[Index];
            if (Sets.FindByPredicate([Entity](const FSparseSet* Set) { return !Set->Contains(Entity); }) != nullptr)
            {
                continue;
            }
            const uint32 Target = static_cast<uint32>(Group->Num++);
            for (FSparseSet* Set : Sets)
            {
                Set->SwapDenseSlots(Set->GetDenseIndex(Entity), Target);
            }
        }
        return Group;
    }

    void FRegistry::PermuteDense(TSpan<FSparseSet* const> Sets, int32 Begin, TSpan<int32> Order)
    {
        for (int32 Start = 0; Start < Order.Num(); ++Start)
        {
            int32 Current = Start;
            int32 Next = Order[Current] - Begin;
            while (Next != Start)
            {
                for (FSparseSet* Set : Sets)
                {
                    Set->SwapDenseSlots(static_cast<uint32>(Begin + Current), static_cast<uint32>(Begin + Next));
                }
                Order[Current] = Begin + Current;
                Current = Next;
                Next = Order[Current] - Begin;
            }
            Order[Current] = Begin + Current;
        }

        for (FSparseSet* Set : Sets)
        {
            Set->BumpWriteEpoch();
        }
    }
}
