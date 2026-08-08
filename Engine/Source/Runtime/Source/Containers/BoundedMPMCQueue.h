#pragma once

#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <immintrin.h>
#endif

namespace Lumina
{
    /**
     * Fixed-capacity multi-producer multi-consumer queue (Vyukov's bounded ring).
     *
     * For pools whose element count is known up front -- a free list of pool indices, a fiber pool --
     * where an unbounded queue is the wrong shape: it cannot hold more than the pool it hands out from,
     * so the only thing its growth machinery buys is allocation. moodycamel pays for the general case
     * with a per-(queue, thread) implicit producer, allocated on that thread's first enqueue and held
     * for the queue's lifetime, plus block allocations as producers fill up. Here the whole ring is one
     * allocation at Initialize() and never allocates again, on any thread, at any depth.
     *
     * Each cell carries a sequence number, so a producer and a consumer claim slots with one CAS each
     * and never touch a shared node list. Capacity is rounded up to a power of two.
     *
     * TryEnqueue fails (returns false) when the write cell has not been republished by its consumer,
     * which is NOT the same thing as "the ring is full". A consumer claims its slot by CAS'ing the read
     * position and only then stores the cell's new sequence; for the whole window in between, a producer
     * landing on that cell sees a stale sequence and refuses -- with a free slot sitting right there.
     * The window is one store wide, but a ring sized to exactly the pool it recycles rides the boundary
     * on every operation, so the idle path (dequeue, find nothing to do, put straight back) hits it
     * constantly.
     *
     * Hence two producer entry points: TryEnqueue for real back-pressure, and Enqueue, which spins
     * through the transient refusal and always succeeds. Pool free lists must use Enqueue -- dropping an
     * item there permanently removes it from the pool, and enough drops starve the owner into deadlock.
     * They should also size the ring with headroom over the pool so the spin is not the common case.
     */
    template<typename T>
    class TBoundedMPMCQueue
    {
    public:

        TBoundedMPMCQueue() = default;
        ~TBoundedMPMCQueue() { Shutdown(); }

        TBoundedMPMCQueue(const TBoundedMPMCQueue&) = delete;
        TBoundedMPMCQueue& operator=(const TBoundedMPMCQueue&) = delete;

        void Initialize(uint32 InCapacity)
        {
            Shutdown();

            uint64 Capacity = 2;
            while (Capacity < InCapacity)
            {
                Capacity <<= 1;
            }

            Cells = static_cast<FCell*>(Memory::Malloc(sizeof(FCell) * Capacity, alignof(FCell)));
            Mask  = Capacity - 1;

            for (uint64 i = 0; i < Capacity; ++i)
            {
                new (&Cells[i]) FCell();
                // Seeded with its own index: a cell is writable when its sequence equals the enqueue
                // position, and readable when it is one past the dequeue position.
                Cells[i].Sequence.store(i, std::memory_order_relaxed);
            }

            EnqueuePos.store(0, std::memory_order_relaxed);
            DequeuePos.store(0, std::memory_order_relaxed);
        }

        void Shutdown()
        {
            if (Cells == nullptr)
            {
                return;
            }

            for (uint64 i = 0; i <= Mask; ++i)
            {
                Cells[i].~FCell();
            }
            Memory::Free(Cells);
            Cells = nullptr;
            Mask  = 0;
        }

        bool IsInitialized() const { return Cells != nullptr; }

        /**
         * Enqueue, retrying through a transient refusal. Never drops the item.
         *
         * Correct only where the caller knows the ring cannot really be full -- a pool free list, where
         * an item only goes back in because it came out. There the refusal is always a consumer mid-claim
         * and resolves in bounded time (its next store), so spinning is short. The yield tail is for the
         * case where that consumer got preempted between its CAS and its store.
         */
        void Enqueue(const T& Item)
        {
            if (LIKELY(TryEnqueue(Item)))
            {
                return;
            }

            for (uint32 Spin = 0; ; ++Spin)
            {
                if (Spin < kEnqueuePauseSpins)
                {
                    _mm_pause();
                }
                else
                {
                    Threading::ThreadYield();
                }

                if (TryEnqueue(Item))
                {
                    return;
                }
            }
        }

        bool TryEnqueue(const T& Item)
        {
            FCell* Cell = nullptr;
            uint64 Pos  = EnqueuePos.load(std::memory_order_relaxed);

            for (;;)
            {
                Cell = &Cells[Pos & Mask];
                const uint64 Sequence = Cell->Sequence.load(std::memory_order_acquire);
                const int64  Diff     = static_cast<int64>(Sequence) - static_cast<int64>(Pos);

                if (Diff == 0)
                {
                    // The slot is ours if we win the position.
                    if (EnqueuePos.compare_exchange_weak(Pos, Pos + 1, std::memory_order_relaxed))
                    {
                        break;
                    }
                }
                else if (Diff < 0)
                {
                    // The cell has not been republished for this lap: either it still holds an unconsumed
                    // item (really full) or a consumer has claimed it and not yet stored its sequence
                    // (transient). Indistinguishable from here -- see Enqueue for who may retry.
                    return false;
                }
                else
                {
                    Pos = EnqueuePos.load(std::memory_order_relaxed);
                }
            }

            Cell->Data = Item;
            Cell->Sequence.store(Pos + 1, std::memory_order_release);
            return true;
        }

        bool TryDequeue(T& Out)
        {
            FCell* Cell = nullptr;
            uint64 Pos  = DequeuePos.load(std::memory_order_relaxed);

            for (;;)
            {
                Cell = &Cells[Pos & Mask];
                const uint64 Sequence = Cell->Sequence.load(std::memory_order_acquire);
                const int64  Diff     = static_cast<int64>(Sequence) - static_cast<int64>(Pos + 1);

                if (Diff == 0)
                {
                    if (DequeuePos.compare_exchange_weak(Pos, Pos + 1, std::memory_order_relaxed))
                    {
                        break;
                    }
                }
                else if (Diff < 0)
                {
                    return false;   // empty
                }
                else
                {
                    Pos = DequeuePos.load(std::memory_order_relaxed);
                }
            }

            Out = Cell->Data;
            Cell->Sequence.store(Pos + Mask + 1, std::memory_order_release);
            return true;
        }

        /** Racy by nature (both ends move independently); for stats and wake heuristics only. */
        uint32 SizeApprox() const
        {
            const uint64 Tail = EnqueuePos.load(std::memory_order_relaxed);
            const uint64 Head = DequeuePos.load(std::memory_order_relaxed);
            return Tail > Head ? static_cast<uint32>(Tail - Head) : 0u;
        }

    private:

        // Pause budget before Enqueue's retry loop starts yielding. Sized for the normal case (a consumer
        // a few instructions from its sequence store); anything longer means it was preempted, and there
        // the OS scheduler, not a hot loop, is what gets it running again.
        static constexpr uint32 kEnqueuePauseSpins = 64;

        struct FCell
        {
            TAtomic<uint64> Sequence;
            T               Data{};
        };

        FCell* Cells = nullptr;
        uint64 Mask  = 0;

        // Opposite ends of the ring are written by different threads every operation; sharing a line
        // between them would turn every enqueue into an invalidation for every consumer.
        alignas(64) TAtomic<uint64> EnqueuePos{0};
        alignas(64) TAtomic<uint64> DequeuePos{0};
    };
}
