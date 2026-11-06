#pragma once

#include "Memory/Memory.h"

#include <atomic>
#include <new>

namespace Lumina::Jobs
{
    // Chase-Lev work-stealing deque: the owner pushes and pops the bottom, thieves take the top.
    template<typename T>
    class TWorkDeque
    {
    public:

        TWorkDeque()
        {
            Ring.store(NewRing(kInitialCapacity), std::memory_order_relaxed);
        }

        TWorkDeque(const TWorkDeque&) = delete;
        TWorkDeque& operator=(const TWorkDeque&) = delete;

        ~TWorkDeque()
        {
            FRing* Live = Ring.load(std::memory_order_relaxed);
            while (Live != nullptr)
            {
                FRing* Older = Live->Older;
                void* Raw = Live;
                Memory::Free(Raw);
                Live = Older;
            }
        }

        FORCEINLINE void Push(T* Item)
        {
            const int64 B = Bottom.load(std::memory_order_relaxed);
            const int64 Tp = Top.load(std::memory_order_acquire);
            FRing* R = Ring.load(std::memory_order_relaxed);
            if (B - Tp > R->Mask)
            {
                R = Grow(R, B, Tp);
            }
            R->Slot(B).store(Item, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            Bottom.store(B + 1, std::memory_order_relaxed);
        }

        // Never pops below Floor, so a nested wait cannot take an outer task's continuation.
        NODISCARD FORCEINLINE T* Pop()
        {
            const int64 B = Bottom.load(std::memory_order_relaxed) - 1;
            if (B < Floor)
            {
                return nullptr;
            }
            FRing* R = Ring.load(std::memory_order_relaxed);
            Bottom.store(B, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64 Tp = Top.load(std::memory_order_relaxed);
            if (Tp > B)
            {
                Bottom.store(B + 1, std::memory_order_relaxed);
                return nullptr;
            }
            T* Item = R->Slot(B).load(std::memory_order_relaxed);
            if (Tp == B)
            {
                if (!Top.compare_exchange_strong(Tp, Tp + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                {
                    Item = nullptr;
                }
                Bottom.store(B + 1, std::memory_order_relaxed);
            }
            return Item;
        }

        NODISCARD FORCEINLINE T* Steal()
        {
            int64 Tp = Top.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const int64 B = Bottom.load(std::memory_order_acquire);
            if (Tp >= B)
            {
                return nullptr;
            }
            FRing* R = Ring.load(std::memory_order_acquire);
            T* Item = R->Slot(Tp).load(std::memory_order_relaxed);
            if (!Top.compare_exchange_strong(Tp, Tp + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                return nullptr;
            }
            return Item;
        }

        NODISCARD FORCEINLINE bool LooksEmpty() const
        {
            return Top.load(std::memory_order_relaxed) >= Bottom.load(std::memory_order_relaxed);
        }

        NODISCARD FORCEINLINE int64 Size() const
        {
            const int64 Count = Bottom.load(std::memory_order_relaxed) - Top.load(std::memory_order_relaxed);
            return Count > 0 ? Count : 0;
        }

        // Owner only: everything pushed from here on sits above the floor.
        NODISCARD int64 RaiseFloor()
        {
            const int64 Previous = Floor;
            Floor = Bottom.load(std::memory_order_relaxed);
            return Previous;
        }

        void RestoreFloor(int64 Previous) { Floor = Previous; }

    private:

        static constexpr int64 kInitialCapacity = 256;

        struct FRing
        {
            int64  Mask  = 0;
            FRing* Older = nullptr;

            FORCEINLINE std::atomic<T*>& Slot(int64 Index) { return reinterpret_cast<std::atomic<T*>*>(this + 1)[Index & Mask]; }
        };

        static FRing* NewRing(int64 Capacity)
        {
            void* Raw = Memory::Malloc(sizeof(FRing) + sizeof(std::atomic<T*>) * static_cast<std::size_t>(Capacity), 64);
            FRing* R = new (Raw) FRing();
            R->Mask = Capacity - 1;
            for (int64 Index = 0; Index < Capacity; ++Index)
            {
                new (&R->Slot(Index)) std::atomic<T*>(nullptr);
            }
            return R;
        }

        // The old ring stays alive until the deque dies, since a thief may still be reading it.
        FRing* Grow(FRing* Old, int64 B, int64 Tp)
        {
            FRing* Bigger = NewRing((Old->Mask + 1) * 2);
            for (int64 Index = Tp; Index < B; ++Index)
            {
                Bigger->Slot(Index).store(Old->Slot(Index).load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            Bigger->Older = Old;
            Ring.store(Bigger, std::memory_order_release);
            return Bigger;
        }

        alignas(64) std::atomic<int64> Top{0};
        alignas(64) std::atomic<int64> Bottom{0};
        alignas(64) std::atomic<FRing*> Ring{nullptr};
        int64 Floor = 0;
    };
}
