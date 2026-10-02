#pragma once
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Core/Singleton/Singleton.h"
#include "Core/Threading/Thread.h"
#include "GUID/GUID.h"


namespace Lumina
{
    class CClass;
    class CPackage;
    class CObjectBase;
}

namespace Lumina
{
    using FObjectHashBucket = TFixedHashSet<CObjectBase*, 4>;

    template<typename TKey>
    using TObjectHashMap = TFixedHashMap<TKey, FObjectHashBucket, 12>;
    
    using FObjectGUIDMap = TFixedHashMap<FGuid, CObjectBase*, 12>; 

    class FObjectHashTables : public TSingleton<FObjectHashTables>
    {
    public:

        void AddObject(CObjectBase* Object);

        void RemoveObject(CObjectBase* Object);

        CObjectBase* FindObject(const FGuid& GUID);
        CObjectBase* FindObject(const FName& Name, CClass* Class);
        CObjectBase* FindObject(const FName& Name, const CPackage* Package);

        void Clear();

    private:

        // Loader threads register objects by the thousand, so one table behind one lock stalled every lookup while it regrew.
        static constexpr uint32 ShardBits = 6;
        static constexpr uint32 ShardCount = 1u << ShardBits;

        struct alignas(64) FGuidShard
        {
            mutable FSharedMutex Mutex;
            FObjectGUIDMap       Objects;
        };

        struct alignas(64) FNameShard
        {
            mutable FSharedMutex   Mutex;
            TObjectHashMap<FName>  Objects;
        };

        template<typename TKey>
        static uint32 ShardOf(const TKey& Key) noexcept
        {
            return static_cast<uint32>((GetTypeHash(Key) * 0x9E3779B97F4A7C15ull) >> (64 - ShardBits));
        }

        FGuidShard GuidShards[ShardCount];
        FNameShard NameShards[ShardCount];
    };
}
