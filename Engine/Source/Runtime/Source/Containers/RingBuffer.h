#pragma once

#include "ContainerTraits.h"
#include "Vector.h"

namespace Lumina::Containers
{
    /** Fixed-capacity queue that overwrites its oldest entry once full; indexing runs oldest to newest. */
    template <typename T, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TRingBuffer
    {
    public:

        using value_type = T;
        using size_type  = size_t;

        TRingBuffer() = default;

        explicit TRingBuffer(size_t InCapacity)
        {
            set_capacity(InCapacity);
        }

        NODISCARD FORCEINLINE size_t size() const noexcept { return Count; }
        NODISCARD FORCEINLINE size_t capacity() const noexcept { return Storage.size(); }
        NODISCARD FORCEINLINE bool empty() const noexcept { return Count == 0; }
        NODISCARD FORCEINLINE bool full() const noexcept { return Count != 0 && Count == Storage.size(); }

        void set_capacity(size_t NewCapacity)
        {
            if (NewCapacity == Storage.size())
            {
                return;
            }

            TVector<T, 0, TAllocator> Rebuilt;
            const size_t Kept = Count < NewCapacity ? Count : NewCapacity;
            Rebuilt.reserve(NewCapacity);

            // Keep the newest entries, since a shrink is meant to drop history rather than recent lines.
            for (size_t Index = Count - Kept; Index < Count; ++Index)
            {
                Rebuilt.push_back((*this)[Index]);
            }
            Rebuilt.resize(NewCapacity);

            Storage = std::move(Rebuilt);
            Count = Kept;
            First = 0;
        }

        void push_back(const T& Value)
        {
            if (Storage.empty())
            {
                return;
            }
            Storage[SlotFor(Count)] = Value;
            Advance();
        }

        void push_back(T&& Value)
        {
            if (Storage.empty())
            {
                return;
            }
            Storage[SlotFor(Count)] = std::move(Value);
            Advance();
        }

        template <typename... TArgs>
        void emplace_back(TArgs&&... Args)
        {
            push_back(T(std::forward<TArgs>(Args)...));
        }

        void clear() noexcept
        {
            Count = 0;
            First = 0;
        }

        NODISCARD FORCEINLINE T& operator[](size_t Index) noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Count);
            return Storage[SlotFor(Index)];
        }

        NODISCARD FORCEINLINE const T& operator[](size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Count);
            return Storage[SlotFor(Index)];
        }

        NODISCARD FORCEINLINE T& front() noexcept { return (*this)[0]; }
        NODISCARD FORCEINLINE const T& front() const noexcept { return (*this)[0]; }
        NODISCARD FORCEINLINE T& back() noexcept { return (*this)[Count - 1]; }
        NODISCARD FORCEINLINE const T& back() const noexcept { return (*this)[Count - 1]; }

    private:

        NODISCARD FORCEINLINE size_t SlotFor(size_t Index) const noexcept
        {
            const size_t Slot = First + Index;
            return Slot < Storage.size() ? Slot : Slot - Storage.size();
        }

        void Advance() noexcept
        {
            if (Count < Storage.size())
            {
                ++Count;
                return;
            }
            First = First + 1 < Storage.size() ? First + 1 : 0;
        }

        TVector<T, 0, TAllocator> Storage;
        size_t Count = 0;
        size_t First = 0;
    };
}

namespace Lumina
{
    template <typename T>
    using TRingBuffer = Containers::TRingBuffer<T>;
}
