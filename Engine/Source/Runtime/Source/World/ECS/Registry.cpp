#include "RuntimePCH.h"
#include "Registry.h"

#include "Core/Assertions/Assert.h"
#include "Core/Object/Class.h"

namespace Lumina::ECS
{
    FComponentTypeRegistry& FComponentTypeRegistry::Get()
    {
        static FComponentTypeRegistry Instance;
        return Instance;
    }

    FComponentTypeRegistry::FComponentTypeRegistry()
    {
        Types.reserve(MaxComponentTypes);
    }

    void FComponentTypeRegistry::BindStruct(FComponentTypeID TypeID, CStruct* Struct)
    {
        if (TypeID < Types.size())
        {
            Types[TypeID]->CachedStruct = Struct;
        }
    }

    FComponentTypeID FComponentTypeRegistry::Acquire(const FComponentTypeInfo& Info)
    {
        if (const FComponentTypeID Existing = FindByName(Info.Name); Existing != InvalidComponentTypeID)
        {
            return Existing;
        }

        ASSERT(Types.size() < InvalidComponentTypeID);
        ASSERT(Types.size() < MaxComponentTypes);

        TUniquePtr<FComponentTypeInfo> Stored = MakeUnique<FComponentTypeInfo>(Info);
        Stored->TypeID = static_cast<FComponentTypeID>(Types.size());

        const FComponentTypeID TypeID = Stored->TypeID;
        Types.push_back(std::move(Stored));
        return TypeID;
    }

    const FComponentTypeInfo& FComponentTypeRegistry::GetInfo(FComponentTypeID TypeID) const
    {
        ASSERT(TypeID < Types.size());
        return *Types[TypeID];
    }

    FComponentTypeID FComponentTypeRegistry::FindByName(const FName& Name) const
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

        for (const TUniquePtr<FComponentTypeInfo>& Info : Types)
        {
            if (Info->GetStruct() == Struct)
            {
                return Info->TypeID;
            }
        }
        return InvalidComponentTypeID;
    }

    size_t FComponentTypeRegistry::Num() const
    {
        return Types.size();
    }



    // Slots past EntityRecords.size() up to NextAppendIndex are handed out but not yet stored, so a reservation never reallocates the records a reader may be walking.
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

        const uint32 Index = Math::Max(AllocState->NextAppendIndex, static_cast<uint32>(EntityRecords.size()));
        ASSERT(Index <= FEntity::MaxIndex);
        AllocState->NextAppendIndex = Index + 1u;
        OutVersion = 0u;
        return Index;
    }

    void FRegistry::EnsureRecordLocked(uint32 Index)
    {
        if (Index >= EntityRecords.size())
        {
            // The slots in between belong to reservations other threads still hold, so they stay unlinked.
            EntityRecords.resize(Index + 1u, FEntity(NoFreeSlot, 0u));
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
            const uint32 Index = Math::Max(AllocState->NextAppendIndex, static_cast<uint32>(EntityRecords.size()));
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

        if (Index >= EntityRecords.size())
        {
            TScopeLock<FMutex> Lock(AllocState->Mutex);

            // Everything between the end and the hint becomes free, linked head-first so Create reuses it.
            const uint32 FirstNew = static_cast<uint32>(EntityRecords.size());
            EntityRecords.resize(Index + 1u);

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
        if (!IsValid(Entity))
        {
            return;
        }

        EntityDestroyed.Broadcast(*this, Entity);
        DetachFromAllStorages(Entity);

        const uint32 Index = Entity.GetIndex();
        {
            TScopeLock<FMutex> Lock(AllocState->Mutex);
            FEntity& Record = EntityRecords[Index];
            Record = FEntity(FreeEntityHead, Record.GetNextVersion());
            FreeEntityHead = Index;
        }
        --LiveEntityCount;
    }

    void FRegistry::DetachFromAllStorages(FEntity Entity)
    {
        for (FSparseSet* Storage : ActiveStorages)
        {
            if (!Storage->Contains(Entity))
            {
                continue;
            }
            Storage->Signals.OnDestroy.Broadcast(*this, Entity);
            Storage->RemoveEntity(Entity);
        }
    }

    void FRegistry::Clear()
    {
        for (FSparseSet* Storage : ActiveStorages)
        {
            Storage->ClearAll();
        }

        EntityRecords.clear();
        FreeEntityHead = NoFreeSlot;
        LiveEntityCount = 0;
        AllocState->NextAppendIndex = 0u;
    }

    void FRegistry::Reserve(size_t EntityCount)
    {
        EntityRecords.reserve(EntityCount);
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
        EntityRecords.swap(Other.EntityRecords);
        OwnedStorages.swap(Other.OwnedStorages);
        StoragesByTypeID.swap(Other.StoragesByTypeID);
        ActiveStorages.swap(Other.ActiveStorages);
        NamedStorages.swap(Other.NamedStorages);
        Context.Swap(Other.Context);

        const uint32 FreeHead = FreeEntityHead;
        FreeEntityHead = Other.FreeEntityHead;
        Other.FreeEntityHead = FreeHead;

        AllocState.Swap(Other.AllocState);

        const size_t LiveCount = LiveEntityCount;
        LiveEntityCount = Other.LiveEntityCount;
        Other.LiveEntityCount = LiveCount;
    }

    uint64 FRegistry::MakeNamedStorageKey(FComponentTypeID TypeID, const FName& Name)
    {
        return (static_cast<uint64>(TypeID) << 32) | static_cast<uint64>(Name.Hash() & 0xFFFFFFFFull);
    }

    FSparseSet* FRegistry::FindNamedStorage(FComponentTypeID TypeID, const FName& Name) const
    {
        const auto It = NamedStorages.find(MakeNamedStorageKey(TypeID, Name));
        return It != NamedStorages.end() ? It->second.get() : nullptr;
    }

    FSparseSet* FRegistry::AssureNamedStorage(FComponentTypeID TypeID, const FName& Name)
    {
        const uint64 Key = MakeNamedStorageKey(TypeID, Name);

        const auto It = NamedStorages.find(Key);
        if (It != NamedStorages.end())
        {
            return It->second.get();
        }

        TUniquePtr<FSparseSet> Storage = MakeUnique<FSparseSet>(FComponentTypeRegistry::Get().GetInfo(TypeID));
        FSparseSet* Raw = Storage.Get();

        NamedStorages.emplace(Key, std::move(Storage));

        // Joins the walk so a destroy detaches from it too, but never the by-type-id index, which one pool owns.
        ActiveStorages.push_back(Raw);
        return Raw;
    }

    void FRegistry::CreateStorage(FComponentTypeID TypeID)
    {
        if (TypeID >= StoragesByTypeID.size())
        {
            StoragesByTypeID.resize(TypeID + 1u, nullptr);
        }

        TUniquePtr<FSparseSet> Storage = MakeUnique<FSparseSet>(FComponentTypeRegistry::Get().GetInfo(TypeID));

        FSparseSet* Raw = Storage.Get();
        StoragesByTypeID[TypeID] = Raw;
        ActiveStorages.push_back(Raw);
        OwnedStorages.push_back(std::move(Storage));
    }
}
