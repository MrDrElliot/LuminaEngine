#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "ScratchArray.h"

#include "Memory/MemoryTracking.h"

#include <atomic>
#include <cstring>

namespace Lumina::ScratchPool
{
    namespace
    {
        constexpr SIZE_T kBlockAlignment = 64;
        constexpr SIZE_T kMinBlockSize   = 4 * 1024;
        constexpr SIZE_T kNumClasses     = 20;

        // Per class rather than per block count, so a 1 MiB class retains many and a 512 MiB class retains few.
        constexpr SIZE_T kRetainedBytesPerClass = 32 * 1024 * 1024;
        constexpr SIZE_T kMinRetainedBlocks     = 2;

        struct FSizeClass
        {
            FMutex Lock;
            void*      Head           = nullptr;
            SIZE_T     RetainedBlocks = 0;
        };

        FSizeClass GClasses[kNumClasses];

        std::atomic<SIZE_T> GLiveBlocks{0};
        std::atomic<SIZE_T> GLiveBytes{0};

        FORCEINLINE SIZE_T CapacityForClass(SIZE_T ClassIndex)
        {
            return kMinBlockSize << ClassIndex;
        }

        // Returns kNumClasses for a size too large to pool, which is the caller's signal to go direct.
        SIZE_T ClassIndexFor(SIZE_T Size)
        {
            SIZE_T Capacity = kMinBlockSize;
            for (SIZE_T Index = 0; Index < kNumClasses; ++Index)
            {
                if (Size <= Capacity)
                {
                    return Index;
                }
                Capacity <<= 1;
            }

            return kNumClasses;
        }

        SIZE_T RetainLimitFor(SIZE_T ClassIndex)
        {
            const SIZE_T Limit = kRetainedBytesPerClass / CapacityForClass(ClassIndex);
            return Limit > kMinRetainedBlocks ? Limit : kMinRetainedBlocks;
        }

        void FreeBlock(void* Block)
        {
            void* Pointer = Block;
            Memory::Free(Pointer);
        }

        void TrackAcquire(SIZE_T Bytes)
        {
            GLiveBlocks.fetch_add(1, std::memory_order_relaxed);
            GLiveBytes.fetch_add(Bytes, std::memory_order_relaxed);
        }
    }

    void* Acquire(SIZE_T Size, SIZE_T& OutCapacity)
    {
        LUMINA_MEMORY_SCOPE("ScratchPool");

        if (Size == 0)
        {
            OutCapacity = 0;
            return nullptr;
        }

        const SIZE_T ClassIndex = ClassIndexFor(Size);

        if (ClassIndex >= kNumClasses)
        {
            OutCapacity = Size;
            TrackAcquire(Size);
            return Memory::Malloc(Size, kBlockAlignment);
        }

        FSizeClass& Class = GClasses[ClassIndex];
        OutCapacity = CapacityForClass(ClassIndex);

        {
            FScopeLock Guard(Class.Lock);
            if (Class.Head != nullptr)
            {
                void* Block = Class.Head;

                // The free list threads itself through the first bytes of each retired block.
                std::memcpy(&Class.Head, Block, sizeof(void*));
                --Class.RetainedBlocks;

                TrackAcquire(OutCapacity);
                return Block;
            }
        }

        TrackAcquire(OutCapacity);
        return Memory::Malloc(OutCapacity, kBlockAlignment);
    }

    void Release(void* Block, SIZE_T Capacity)
    {
        if (Block == nullptr)
        {
            return;
        }

        GLiveBlocks.fetch_sub(1, std::memory_order_relaxed);
        GLiveBytes.fetch_sub(Capacity, std::memory_order_relaxed);

        const SIZE_T ClassIndex = ClassIndexFor(Capacity);

        if (ClassIndex < kNumClasses)
        {
            FSizeClass& Class = GClasses[ClassIndex];

            FScopeLock Guard(Class.Lock);
            if (Class.RetainedBlocks < RetainLimitFor(ClassIndex))
            {
                std::memcpy(Block, &Class.Head, sizeof(void*));
                Class.Head = Block;
                ++Class.RetainedBlocks;
                return;
            }
        }

        FreeBlock(Block);
    }

    void Trim()
    {
        for (SIZE_T Index = 0; Index < kNumClasses; ++Index)
        {
            FSizeClass& Class = GClasses[Index];

            void* Head = nullptr;
            {
                FScopeLock Guard(Class.Lock);
                Head = Class.Head;
                Class.Head = nullptr;
                Class.RetainedBlocks = 0;
            }

            while (Head != nullptr)
            {
                void* Next = nullptr;
                std::memcpy(&Next, Head, sizeof(void*));
                FreeBlock(Head);
                Head = Next;
            }
        }
    }

    FStats GetStats()
    {
        FStats Stats;

        for (SIZE_T Index = 0; Index < kNumClasses; ++Index)
        {
            FSizeClass& Class = GClasses[Index];

            FScopeLock Guard(Class.Lock);
            Stats.RetainedBlocks += Class.RetainedBlocks;
            Stats.RetainedBytes  += Class.RetainedBlocks * CapacityForClass(Index);
        }

        Stats.LiveBlocks = GLiveBlocks.load(std::memory_order_relaxed);
        Stats.LiveBytes  = GLiveBytes.load(std::memory_order_relaxed);
        return Stats;
    }
}
