#include "RuntimePCH.h"
#include "ObjectAllocator.h"

#include "ObjectBase.h"
#include "Containers/HashPrimitives.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"

namespace Lumina
{
    /** Global CObject allocator */
    RUNTIME_API FCObjectAllocator Global::GCObjectAllocator;

    FCObjectAllocator::FCounterShard& FCObjectAllocator::ThreadCounters()
    {
        return Counters[Containers::MixHash64(Threading::GetThreadID()) % kCounterShards];
    }

    void* FCObjectAllocator::AllocateCObject(uint32 Size, uint32 Alignment)
    {
        void* Storage = Memory::MallocUntracked(Size, Alignment);

        FCounterShard& Shard = ThreadCounters();
        Shard.LiveBytes.fetch_add((int64)Memory::GetAllocationSize(Storage), std::memory_order_relaxed);
        Shard.Allocs.fetch_add(1, std::memory_order_relaxed);
        return Storage;
    }

    void FCObjectAllocator::FreeCObject(CObjectBase* Ptr)
    {
        if (Ptr == nullptr)
        {
            return;
        }

        Ptr->~CObjectBase();

        // A shard can go negative when another thread made the allocation, which the sum in GetStats absorbs.
        FCounterShard& Shard = ThreadCounters();
        Shard.LiveBytes.fetch_sub((int64)Memory::GetAllocationSize(Ptr), std::memory_order_relaxed);
        Shard.Frees.fetch_add(1, std::memory_order_relaxed);

        Memory::FreeUntracked(Ptr);
    }

    Memory::FCountedCategoryStats FCObjectAllocator::GetStats() const
    {
        int64  LiveBytes = 0;
        uint64 Allocs    = 0;
        uint64 Frees     = 0;
        for (const FCounterShard& Shard : Counters)
        {
            LiveBytes += Shard.LiveBytes.load(std::memory_order_relaxed);
            Allocs    += Shard.Allocs.load(std::memory_order_relaxed);
            Frees     += Shard.Frees.load(std::memory_order_relaxed);
        }

        Memory::FCountedCategoryStats Stats;
        Stats.LiveBytes   = LiveBytes > 0 ? (uint64)LiveBytes : 0u;
        Stats.LiveCount   = Allocs > Frees ? Allocs - Frees : 0u;
        Stats.TotalAllocs = Allocs;
        Stats.TotalFrees  = Frees;
        return Stats;
    }
}
