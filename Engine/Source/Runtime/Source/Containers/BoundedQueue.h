#pragma once

#include "Core/Templates/LuminaTemplate.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/Construct.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <immintrin.h>
#endif

namespace Lumina
{
    /** How many threads may touch each end. Naming the guarantee lets the ring drop what it cannot need. */
    enum class EQueueConcurrency : uint8
    {
        SPSC,
        MPSC,
        SPMC,
        MPMC,
    };

    namespace Private
    {
        constexpr uint64 RoundUpToPowerOfTwo(uint64 Value)
        {
            uint64 Result = 2;
            while (Result < Value)
            {
                Result <<= 1;
            }
            return Result;
        }

        /** Carries the lap number a reader or writer must see before the slot is theirs. */
        template <typename T>
        struct TSequencedCell
        {
            TAtomic<uint64> Sequence;
            T               Data{};
        };

        /** SPSC publishes strictly in order, so the two cursors already say everything a cell would. */
        template <typename T>
        struct TPlainCell
        {
            T Data{};
        };
    }

    /** Bounded ring specialized on how many threads touch each end; FixedCapacity 0 sizes it at runtime. */
    template <typename T, EQueueConcurrency Concurrency = EQueueConcurrency::MPMC, uint32 FixedCapacity = 0>
    class TBoundedQueue
    {
        static constexpr bool bMultiProducer = Concurrency == EQueueConcurrency::MPMC || Concurrency == EQueueConcurrency::MPSC;
        static constexpr bool bMultiConsumer = Concurrency == EQueueConcurrency::MPMC || Concurrency == EQueueConcurrency::SPMC;

        // Only a strictly ordered single producer paired with a single consumer can skip the sequence.
        static constexpr bool bSequenced = bMultiProducer || bMultiConsumer;

        // A compile-time capacity moves the storage inline and folds the index math into a constant.
        static constexpr bool bFixed = FixedCapacity != 0;

        using FCell = std::conditional_t<bSequenced, Private::TSequencedCell<T>, Private::TPlainCell<T>>;

        static constexpr uint64 kFixedCapacity = bFixed ? Private::RoundUpToPowerOfTwo(FixedCapacity) : 1;
        static constexpr uint64 kFixedMask     = kFixedCapacity - 1;

    public:

        TBoundedQueue()
        {
            if constexpr (bFixed)
            {
                ResetSequences();
            }
        }

        ~TBoundedQueue() { Shutdown(); }

        TBoundedQueue(const TBoundedQueue&) = delete;
        TBoundedQueue& operator=(const TBoundedQueue&) = delete;

        static constexpr bool IsFixedCapacity() { return bFixed; }

        /** A no-op on a fixed-capacity ring, so a call site can read the same across both. */
        void Initialize(uint32 InCapacity)
        {
            if constexpr (bFixed)
            {
                (void)InCapacity;
            }
            else
            {
                Shutdown();

                const uint64 Capacity = Private::RoundUpToPowerOfTwo(InCapacity);

                HeapCells   = static_cast<FCell*>(Memory::Malloc(sizeof(FCell) * Capacity, alignof(FCell)));
                RuntimeMask = Capacity - 1;

                for (uint64 i = 0; i < Capacity; ++i)
                {
                    Memory::ConstructAt(HeapCells + i);
                }

                ResetSequences();
            }
        }

        void Shutdown()
        {
            if constexpr (!bFixed)
            {
                if (HeapCells == nullptr)
                {
                    return;
                }

                for (uint64 i = 0; i <= RuntimeMask; ++i)
                {
                    Memory::DestroyAt(HeapCells + i);
                }
                Memory::Free(HeapCells);
                HeapCells   = nullptr;
                RuntimeMask = 0;
            }
        }

        bool IsInitialized() const
        {
            if constexpr (bFixed)
            {
                return true;
            }
            else
            {
                return HeapCells != nullptr;
            }
        }

        uint64 GetCapacity() const { return Mask() + 1; }

        /** Spins through a transient refusal and always succeeds, which is what a pool free list needs. */
        void Enqueue(const T& Item) { EnqueueInternal(Item); }

        void Enqueue(T&& Item) { EnqueueInternal(Move(Item)); }

        /** Multi-producer refusal can also mean a consumer is mid-claim, so it does not prove fullness. */
        bool TryEnqueue(const T& Item) { return TryEnqueueInternal(Item); }

        bool TryEnqueue(T&& Item) { return TryEnqueueInternal(Move(Item)); }

        bool TryDequeue(T& Out)
        {
            if constexpr (bSequenced)
            {
                return TryDequeueSequenced(Out);
            }
            else
            {
                return TryDequeueOrdered(Out);
            }
        }

        /** Racy by nature for the multi shapes; for stats and wake heuristics only. */
        uint32 SizeApprox() const
        {
            const uint64 Tail = EnqueuePos.load(Atomic::MemoryOrderRelaxed);
            const uint64 Head = DequeuePos.load(Atomic::MemoryOrderRelaxed);
            return Tail > Head ? static_cast<uint32>(Tail - Head) : 0u;
        }

    private:

        // Long enough for a consumer a few instructions from its sequence store, past which it was preempted.
        static constexpr uint32 kEnqueuePauseSpins = 64;

        static constexpr size_t kCacheLine = Threading::kCacheLineSize;

        FORCEINLINE uint64 Mask() const
        {
            if constexpr (bFixed)
            {
                return kFixedMask;
            }
            else
            {
                return RuntimeMask;
            }
        }

        FORCEINLINE FCell* CellStorage()
        {
            if constexpr (bFixed)
            {
                return InlineCells;
            }
            else
            {
                return HeapCells;
            }
        }

        void ResetSequences()
        {
            if constexpr (bSequenced)
            {
                FCell* Storage = CellStorage();
                for (uint64 i = 0; i <= Mask(); ++i)
                {
                    // Seeded with its own index, so writable means sequence equals the enqueue position.
                    Storage[i].Sequence.store(i, Atomic::MemoryOrderRelaxed);
                }
            }

            EnqueuePos.store(0, Atomic::MemoryOrderRelaxed);
            DequeuePos.store(0, Atomic::MemoryOrderRelaxed);
            CachedHead = 0;
            CachedTail = 0;
        }

        template <typename TItem>
        void EnqueueInternal(TItem&& Item)
        {
            if (LIKELY(TryEnqueueInternal(std::forward<TItem>(Item))))
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

                if (TryEnqueueInternal(std::forward<TItem>(Item)))
                {
                    return;
                }
            }
        }

        template <typename TItem>
        FORCEINLINE bool TryEnqueueInternal(TItem&& Item)
        {
            if constexpr (bSequenced)
            {
                return TryEnqueueSequenced(std::forward<TItem>(Item));
            }
            else
            {
                return TryEnqueueOrdered(std::forward<TItem>(Item));
            }
        }

        template <typename TItem>
        bool TryEnqueueOrdered(TItem&& Item)
        {
            const uint64 Tail = EnqueuePos.load(Atomic::MemoryOrderRelaxed);

            // Refreshed only when the ring looks full, so the steady state never reads the consumer's line.
            if (Tail - CachedHead > Mask())
            {
                CachedHead = DequeuePos.load(Atomic::MemoryOrderAcquire);
                if (Tail - CachedHead > Mask())
                {
                    return false;
                }
            }

            CellStorage()[Tail & Mask()].Data = std::forward<TItem>(Item);
            EnqueuePos.store(Tail + 1, Atomic::MemoryOrderRelease);
            return true;
        }

        bool TryDequeueOrdered(T& Out)
        {
            const uint64 Head = DequeuePos.load(Atomic::MemoryOrderRelaxed);

            // Refreshed only when the ring looks empty, so the steady state never reads the producer's line.
            if (Head == CachedTail)
            {
                CachedTail = EnqueuePos.load(Atomic::MemoryOrderAcquire);
                if (Head == CachedTail)
                {
                    return false;
                }
            }

            Out = Move(CellStorage()[Head & Mask()].Data);
            DequeuePos.store(Head + 1, Atomic::MemoryOrderRelease);
            return true;
        }

        template <typename TItem>
        bool TryEnqueueSequenced(TItem&& Item)
        {
            FCell* Storage        = CellStorage();
            const uint64 CellMask = Mask();

            FCell* Cell = nullptr;
            uint64 Pos  = EnqueuePos.load(Atomic::MemoryOrderRelaxed);

            for (;;)
            {
                Cell = &Storage[Pos & CellMask];
                const uint64 Sequence = Cell->Sequence.load(Atomic::MemoryOrderAcquire);
                const int64  Diff     = static_cast<int64>(Sequence) - static_cast<int64>(Pos);

                if (Diff == 0)
                {
                    if constexpr (bMultiProducer)
                    {
                        if (EnqueuePos.compare_exchange_weak(Pos, Pos + 1, Atomic::MemoryOrderRelaxed))
                        {
                            break;
                        }
                    }
                    else
                    {
                        // Sole producer, so nothing else can move the position out from under us.
                        EnqueuePos.store(Pos + 1, Atomic::MemoryOrderRelaxed);
                        break;
                    }
                }
                else if (Diff < 0)
                {
                    // Not republished for this lap, which is either really full or a consumer mid-claim.
                    return false;
                }
                else
                {
                    Pos = EnqueuePos.load(Atomic::MemoryOrderRelaxed);
                }
            }

            Cell->Data = std::forward<TItem>(Item);
            Cell->Sequence.store(Pos + 1, Atomic::MemoryOrderRelease);
            return true;
        }

        bool TryDequeueSequenced(T& Out)
        {
            FCell* Storage        = CellStorage();
            const uint64 CellMask = Mask();

            FCell* Cell = nullptr;
            uint64 Pos  = DequeuePos.load(Atomic::MemoryOrderRelaxed);

            for (;;)
            {
                Cell = &Storage[Pos & CellMask];
                const uint64 Sequence = Cell->Sequence.load(Atomic::MemoryOrderAcquire);
                const int64  Diff     = static_cast<int64>(Sequence) - static_cast<int64>(Pos + 1);

                if (Diff == 0)
                {
                    if constexpr (bMultiConsumer)
                    {
                        if (DequeuePos.compare_exchange_weak(Pos, Pos + 1, Atomic::MemoryOrderRelaxed))
                        {
                            break;
                        }
                    }
                    else
                    {
                        // Sole consumer, so nothing else can move the position out from under us.
                        DequeuePos.store(Pos + 1, Atomic::MemoryOrderRelaxed);
                        break;
                    }
                }
                else if (Diff < 0)
                {
                    return false;
                }
                else
                {
                    Pos = DequeuePos.load(Atomic::MemoryOrderRelaxed);
                }
            }

            // Moved, so an owning payload is released now rather than pinned until the cell laps.
            Out = Move(Cell->Data);
            Cell->Sequence.store(Pos + CellMask + 1, Atomic::MemoryOrderRelease);
            return true;
        }

        struct FEmptyStorage {};

        std::conditional_t<bFixed, FCell[kFixedCapacity], FEmptyStorage> InlineCells{};
        std::conditional_t<bFixed, FEmptyStorage, FCell*> HeapCells{};

        // Opposite ends are written by different threads, so sharing a line makes every enqueue invalidate.
        alignas(kCacheLine) TAtomic<uint64> EnqueuePos{0};

        // Producer-owned, so the cached opposite cursor rides the producer's line.
        uint64 CachedHead = 0;

        alignas(kCacheLine) TAtomic<uint64> DequeuePos{0};
        uint64 CachedTail = 0;

        uint64 RuntimeMask = 0;
    };

    template <typename T, uint32 FixedCapacity = 0>
    using TBoundedMPMCQueue = TBoundedQueue<T, EQueueConcurrency::MPMC, FixedCapacity>;

    template <typename T, uint32 FixedCapacity = 0>
    using TBoundedMPSCQueue = TBoundedQueue<T, EQueueConcurrency::MPSC, FixedCapacity>;

    template <typename T, uint32 FixedCapacity = 0>
    using TBoundedSPMCQueue = TBoundedQueue<T, EQueueConcurrency::SPMC, FixedCapacity>;

    template <typename T, uint32 FixedCapacity = 0>
    using TBoundedSPSCQueue = TBoundedQueue<T, EQueueConcurrency::SPSC, FixedCapacity>;
}
