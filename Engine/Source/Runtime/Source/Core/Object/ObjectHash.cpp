#include "RuntimePCH.h"
#include "ObjectHash.h"

#include "ObjectBase.h"
#include "Package/Package.h"

namespace Lumina
{
    FObjectHashTables& FObjectHashTables::Get()
    {
        static FObjectHashTables Instance;
        return Instance;
    }

    void FObjectIndexTable::Insert(CObjectBase* Object, uint64 Hash)
    {
        // Three quarters full at most, which keeps the linear probe runs short.
        if ((uint64)(Count + 1) * 4 > (uint64)Slots.Num() * 3)
        {
            Grow();
        }

        const int32 Mask = Slots.Num() - 1;
        int32 Index = static_cast<int32>(Hash & Mask);
        while (Slots[Index].Object != nullptr)
        {
            Index = (Index + 1) & Mask;
        }

        Slots[Index] = { Object, Hash };
        ++Count;
    }

    bool FObjectIndexTable::Remove(const CObjectBase* Object, uint64 Hash)
    {
        if (Slots.IsEmpty())
        {
            return false;
        }

        const int32 Mask = Slots.Num() - 1;
        int32 Hole = static_cast<int32>(Hash & Mask);
        while (Slots[Hole].Object != Object)
        {
            if (Slots[Hole].Object == nullptr)
            {
                return false;
            }
            Hole = (Hole + 1) & Mask;
        }

        // Shifting the run back instead of leaving a tombstone keeps every probe ending at the first empty slot.
        for (int32 Next = (Hole + 1) & Mask; Slots[Next].Object != nullptr; Next = (Next + 1) & Mask)
        {
            const int32 Home = static_cast<int32>(Slots[Next].Hash & Mask);
            if (((Next - Home) & Mask) >= ((Next - Hole) & Mask))
            {
                Slots[Hole] = Slots[Next];
                Hole = Next;
            }
        }

        Slots[Hole] = {};
        --Count;
        return true;
    }

    void FObjectIndexTable::Grow()
    {
        TVector<FSlot> Old = Move(Slots);
        Slots.Reset();
        Slots.SetNum(Old.IsEmpty() ? 16 : Old.Num() * 2);

        const int32 Mask = Slots.Num() - 1;
        for (const FSlot& Slot : Old)
        {
            if (Slot.Object == nullptr)
            {
                continue;
            }

            int32 Index = static_cast<int32>(Slot.Hash & Mask);
            while (Slots[Index].Object != nullptr)
            {
                Index = (Index + 1) & Mask;
            }
            Slots[Index] = Slot;
        }
    }

    void FObjectIndexTable::Clear()
    {
        Slots = {};
        Count = 0;
    }

    void FObjectHashTables::AddObject(CObjectBase* Object)
    {
        LUMINA_PROFILE_SCOPE();

        const FGuid& ObjectGUID = Object->GetGUID();
        {
            const uint64 Hash = HashOf(ObjectGUID);
            FShard& Shard = GuidShards[ShardOf(Hash)];
            FWriteScopeLock Lock(Shard.Mutex);
            ASSERT(Shard.Objects.FindFirst(Hash, [&](const CObjectBase* Other) { return Other->GetGUID() == ObjectGUID; }) == nullptr);
            Shard.Objects.Insert(Object, Hash);
        }
        {
            const uint64 Hash = HashOf(Object->GetName());
            FShard& Shard = NameShards[ShardOf(Hash)];
            FWriteScopeLock Lock(Shard.Mutex);
            Shard.Objects.Insert(Object, Hash);
        }
    }

    void FObjectHashTables::RemoveObject(CObjectBase* Object)
    {
        LUMINA_PROFILE_SCOPE();

        {
            const uint64 Hash = HashOf(Object->GetGUID());
            FShard& Shard = GuidShards[ShardOf(Hash)];
            FWriteScopeLock Lock(Shard.Mutex);
            const bool bRemoved = Shard.Objects.Remove(Object, Hash);
            ASSERT(bRemoved);
        }
        {
            // Uses the object's current name, since HandleNameChange removes before it mutates.
            const uint64 Hash = HashOf(Object->GetName());
            FShard& Shard = NameShards[ShardOf(Hash)];
            FWriteScopeLock Lock(Shard.Mutex);
            Shard.Objects.Remove(Object, Hash);
        }
    }

    CObjectBase* FObjectHashTables::FindObject(const FGuid& GUID)
    {
        LUMINA_PROFILE_SCOPE();
        const uint64 Hash = HashOf(GUID);
        const FShard& Shard = GuidShards[ShardOf(Hash)];
        FReadScopeLock Lock(Shard.Mutex);

        CObjectBase* Found = Shard.Objects.FindFirst(Hash, [&](const CObjectBase* Object) { return Object->GetGUID() == GUID; });
        return (Found != nullptr && !Found->HasAnyFlag(OF_MarkedDestroy)) ? Found : nullptr;
    }

    CObjectBase* FObjectHashTables::FindObject(const FName& Name, CClass* Class)
    {
        LUMINA_PROFILE_SCOPE();
        const uint64 Hash = HashOf(Name);
        const FShard& Shard = NameShards[ShardOf(Hash)];
        FReadScopeLock Lock(Shard.Mutex);

        // A subclass matches, since a minted class answers as a CScriptClass and FindObject<T> asks for a base.
        return Shard.Objects.FindFirst(Hash, [&](const CObjectBase* Object)
        {
            return Object->GetName() == Name && Object->GetClass() != nullptr && Object->GetClass()->IsChildOf(Class)
                && !Object->HasAnyFlag(OF_MarkedDestroy);
        });
    }

    CObjectBase* FObjectHashTables::FindObject(const FName& Name, const CPackage* Package)
    {
        LUMINA_PROFILE_SCOPE();
        const uint64 Hash = HashOf(Name);
        const FShard& Shard = NameShards[ShardOf(Hash)];
        FReadScopeLock Lock(Shard.Mutex);

        return Shard.Objects.FindFirst(Hash, [&](const CObjectBase* Object)
        {
            return Object->GetName() == Name && Object->GetPackage() == Package && !Object->HasAnyFlag(OF_MarkedDestroy);
        });
    }

    void FObjectHashTables::Clear()
    {
        for (FShard& Shard : GuidShards)
        {
            FWriteScopeLock Lock(Shard.Mutex);
            Shard.Objects.Clear();
        }
        for (FShard& Shard : NameShards)
        {
            FWriteScopeLock Lock(Shard.Mutex);
            Shard.Objects.Clear();
        }
    }
}
