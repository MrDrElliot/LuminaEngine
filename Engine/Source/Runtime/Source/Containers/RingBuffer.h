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
        using size_type  = int32;

        TRingBuffer() = default;

        explicit TRingBuffer(size_type InCapacity)
        {
            SetCapacity(InCapacity);
        }

        NODISCARD FORCEINLINE int32 Num() const noexcept { return Count; }
        NODISCARD FORCEINLINE int32 Max() const noexcept { return Storage.Num(); }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Count == 0; }
        NODISCARD FORCEINLINE bool IsFull() const noexcept { return Count != 0 && Count == Storage.Num(); }

        void SetCapacity(size_type NewCapacity)
        {
            LUMINA_CONTAINER_CHECK(NewCapacity >= 0);
            if (NewCapacity == Storage.Num())
            {
                return;
            }

            TVector<T, 0, TAllocator> Rebuilt;
            const int32 Kept = Count < NewCapacity ? Count : NewCapacity;
            Rebuilt.Reserve(NewCapacity);

            // Keep the newest entries, since a shrink is meant to drop history rather than recent lines; the old storage goes, so they move.
            for (int32 Index = Count - Kept; Index < Count; ++Index)
            {
                Rebuilt.Add(std::move((*this)[Index]));
            }
            Rebuilt.SetNum(NewCapacity);

            Storage = std::move(Rebuilt);
            Count = Kept;
            Head = 0;
        }

        void Add(const T& Value)
        {
            if (Storage.IsEmpty())
            {
                return;
            }
            Storage[SlotFor(Count)] = Value;
            Advance();
        }

        void Add(T&& Value)
        {
            if (Storage.IsEmpty())
            {
                return;
            }
            Storage[SlotFor(Count)] = std::move(Value);
            Advance();
        }

        template <typename... TArgs>
        void Emplace(TArgs&&... Args)
        {
            Add(T(std::forward<TArgs>(Args)...));
        }

        // Every slot stays constructed, so a cleared one is reset to release what it held rather than keep it until overwritten.
        void Reset() noexcept
        {
            for (int32 Index = 0; Index < Count; ++Index)
            {
                Storage[SlotFor(Index)] = T();
            }
            Count = 0;
            Head = 0;
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto operator[](this TSelf&& Self, int32 Index) noexcept -> TLikeSelf<TSelf, T>&
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Self.Count);
            return Self.Storage[Self.SlotFor(Index)];
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto First(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>& { return Self[0]; }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto Last(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>& { return Self[Self.Count - 1]; }

    private:

        NODISCARD FORCEINLINE int32 SlotFor(int32 Index) const noexcept
        {
            const int32 Slot = Head + Index;
            return Slot < Storage.Num() ? Slot : Slot - Storage.Num();
        }

        void Advance() noexcept
        {
            if (Count < Storage.Num())
            {
                ++Count;
                return;
            }
            Head = Head + 1 < Storage.Num() ? Head + 1 : 0;
        }

        TVector<T, 0, TAllocator> Storage;
        int32 Count = 0;
        int32 Head = 0;
    };
}

namespace Lumina
{
    template <typename T>
    using TRingBuffer = Containers::TRingBuffer<T>;
}
