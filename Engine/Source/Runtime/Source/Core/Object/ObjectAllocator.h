#pragma once
#include "Core/Threading/Atomic.h"
#include "Memory/MemoryTracking.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObjectBase;
}

namespace Lumina
{
    // CObjects skip the per-allocation tracking ledger and are counted here instead, so the tracker never takes a lock for one.
    class FCObjectAllocator
    {
    public:

        FCObjectAllocator() = default;
        ~FCObjectAllocator() = default;
        FCObjectAllocator(const FCObjectAllocator&) = delete;
        FCObjectAllocator& operator=(const FCObjectAllocator&) = delete;
        FCObjectAllocator(FCObjectAllocator&&) = delete;
        FCObjectAllocator& operator=(FCObjectAllocator&&) = delete;

        // Raw allocation that does not construct.
        RUNTIME_API void* AllocateCObject(uint32 Size, uint32 Alignment);

        // Runs the destructor, then returns the memory.
        RUNTIME_API void FreeCObject(CObjectBase* Ptr);

        RUNTIME_API Memory::FCountedCategoryStats GetStats() const;

    private:

        // Spread across threads, so a parallel load is not one contended cache line, and summed only when read.
        static constexpr uint32 kCounterShards = 16;

        struct alignas(64) FCounterShard
        {
            TAtomic<int64>  LiveBytes{0};
            TAtomic<uint64> Allocs{0};
            TAtomic<uint64> Frees{0};
        };

        FCounterShard& ThreadCounters();

        FCounterShard Counters[kCounterShards];
    };

    namespace Global
    {
        extern RUNTIME_API FCObjectAllocator GCObjectAllocator;
    }

}
