#pragma once

#include "BoundedQueue.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Sync.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    /** Unbounded queue that never drops, built as a lock-free ring with a locked spill behind it. */
    template <typename T>
    class TConcurrentQueue
    {
    public:

        // Big enough that the spill is the exception; overflowing it costs a lock, never an item.
        static constexpr uint32 kDefaultRingCapacity = 1024;

        explicit TConcurrentQueue(uint32 RingCapacity = kDefaultRingCapacity)
        {
            Ring.Initialize(RingCapacity);
        }

        TConcurrentQueue(const TConcurrentQueue&) = delete;
        TConcurrentQueue& operator=(const TConcurrentQueue&) = delete;

        void Enqueue(const T& Item) { EnqueueInternal(Item); }
        void Enqueue(T&& Item) { EnqueueInternal(Move(Item)); }

        /** Always succeeds because the queue is unbounded, and exists so generic code has one spelling. */
        bool TryEnqueue(const T& Item) { EnqueueInternal(Item); return true; }

        bool TryEnqueue(T&& Item) { EnqueueInternal(Move(Item)); return true; }

        void EnqueueBulk(const T* Items, size_t Count)
        {
            for (size_t i = 0; i < Count; ++i)
            {
                EnqueueInternal(Items[i]);
            }
        }

        bool TryDequeue(T& Out)
        {
            if (LIKELY(Ring.TryDequeue(Out)))
            {
                return true;
            }

            if (SpillCount.load(Atomic::MemoryOrderAcquire) == 0)
            {
                return false;
            }

            return DequeueFromSpill(Out);
        }

        size_t DequeueBulk(T* Out, size_t MaxCount)
        {
            size_t Count = 0;
            while (Count < MaxCount && TryDequeue(Out[Count]))
            {
                ++Count;
            }
            return Count;
        }

        /** Racy by nature; for stats and wake heuristics only. */
        uint32 SizeApprox() const
        {
            return Ring.SizeApprox() + SpillCount.load(Atomic::MemoryOrderRelaxed);
        }

    private:

        template <typename TItem>
        void EnqueueInternal(TItem&& Item)
        {
            // Anything already spilled has to leave first, or a burst would overtake its own earlier items.
            if (LIKELY(SpillCount.load(Atomic::MemoryOrderAcquire) == 0))
            {
                if (LIKELY(Ring.TryEnqueue(std::forward<TItem>(Item))))
                {
                    return;
                }
            }

            FScopeLock Lock(SpillLock);
            Spill.push_back(std::forward<TItem>(Item));
            SpillCount.store((uint32)(Spill.size() - SpillHead), Atomic::MemoryOrderRelease);
        }

        /** Taken straight off the spill rather than refilled into the ring, so producers keep the ring. */
        bool DequeueFromSpill(T& Out)
        {
            FScopeLock Lock(SpillLock);

            if (SpillHead >= Spill.size())
            {
                return false;
            }

            Out = Move(Spill[SpillHead]);
            ++SpillHead;

            // Compacted by index rather than erased from the front, which would be a copy per dequeue.
            if (SpillHead >= Spill.size())
            {
                Spill.clear();
                SpillHead = 0;
            }
            else if (SpillHead >= 1024 && SpillHead * 2 >= Spill.size())
            {
                Spill.erase(Spill.begin(), Spill.begin() + (ptrdiff_t)SpillHead);
                SpillHead = 0;
            }

            SpillCount.store((uint32)(Spill.size() - SpillHead), Atomic::MemoryOrderRelease);
            return true;
        }

        TBoundedQueue<T, EQueueConcurrency::MPMC> Ring;

        mutable FMutex  SpillLock;
        TVector<T>      Spill;
        size_t          SpillHead = 0;
        TAtomic<uint32> SpillCount{0};
    };
}
