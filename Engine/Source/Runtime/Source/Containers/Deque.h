#pragma once

#include <compare>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "ElementOps.h"
#include "Memory/Construct.h"

namespace Lumina::Containers
{
    /** Double-ended queue over one circular buffer, so both ends are O(1) and elements stay in one block. */
    template <typename T, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TDeque
    {
    public:

        using value_type      = T;
        using size_type       = int32;
        using difference_type = ptrdiff_t;
        using reference       = T&;
        using const_reference = const T&;
        using pointer         = T*;
        using const_pointer   = const T*;

        template <bool bConst>
        class TIterator
        {
            friend class TDeque;
            template <bool> friend class TIterator;

            using FOwner = std::conditional_t<bConst, const TDeque, TDeque>;

        public:

            using iterator_category = std::random_access_iterator_tag;
            using value_type        = T;
            using difference_type   = ptrdiff_t;
            using reference         = std::conditional_t<bConst, const T&, T&>;
            using pointer           = std::conditional_t<bConst, const T*, T*>;

            TIterator() noexcept = default;

            template <bool bOther>
            requires (bConst && !bOther)
            TIterator(const TIterator<bOther>& Other) noexcept : Owner(Other.Owner), Index(Other.Index) {}

            NODISCARD reference operator*() const noexcept { return (*Owner)[Index]; }
            NODISCARD pointer operator->() const noexcept { return &(*Owner)[Index]; }

            TIterator& operator++() noexcept { ++Index; return *this; }
            TIterator operator++(int) noexcept { TIterator Copy = *this; ++Index; return Copy; }
            TIterator& operator--() noexcept { --Index; return *this; }
            TIterator operator--(int) noexcept { TIterator Copy = *this; --Index; return Copy; }

            TIterator& operator+=(difference_type Offset) noexcept { Index += static_cast<int32>(Offset); return *this; }
            TIterator& operator-=(difference_type Offset) noexcept { Index -= static_cast<int32>(Offset); return *this; }

            NODISCARD reference operator[](difference_type Offset) const noexcept { return (*Owner)[Index + static_cast<int32>(Offset)]; }

            NODISCARD friend TIterator operator+(TIterator It, difference_type Offset) noexcept { return It += Offset; }
            NODISCARD friend TIterator operator+(difference_type Offset, TIterator It) noexcept { return It += Offset; }
            NODISCARD friend TIterator operator-(TIterator It, difference_type Offset) noexcept { return It -= Offset; }

            NODISCARD friend difference_type operator-(const TIterator& Left, const TIterator& Right) noexcept
            {
                return static_cast<difference_type>(Left.Index) - static_cast<difference_type>(Right.Index);
            }

            NODISCARD friend bool operator==(const TIterator& Left, const TIterator& Right) noexcept
            {
                return Left.Index == Right.Index && Left.Owner == Right.Owner;
            }

            // Ordered by position, which is all random access needs; comparing iterators of two deques means nothing.
            NODISCARD friend std::strong_ordering operator<=>(const TIterator& Left, const TIterator& Right) noexcept
            {
                return Left.Index <=> Right.Index;
            }

        private:

            TIterator(FOwner* InOwner, int32 InIndex) noexcept : Owner(InOwner), Index(InIndex) {}

            FOwner* Owner = nullptr;
            int32  Index = 0;
        };

        using iterator       = TIterator<false>;
        using const_iterator = TIterator<true>;

        static_assert(std::random_access_iterator<iterator> && std::random_access_iterator<const_iterator>);

        TDeque() noexcept = default;

        explicit TDeque(size_type InitialCapacity) { Reserve(InitialCapacity); }

        TDeque(const TDeque& Other)
        {
            Reserve(Other.Count);
            for (int32 Index = 0; Index < Other.Count; ++Index)
            {
                PushLast(Other[Index]);
            }
        }

        TDeque(TDeque&& Other) noexcept
            : Storage(Other.Storage)
            , Cap(Other.Cap)
            , Head(Other.Head)
            , Count(Other.Count)
        {
            Other.AdoptEmpty();
        }

        ~TDeque() { ReleaseBlock(); }

        TDeque& operator=(const TDeque& Other)
        {
            if (this != &Other)
            {
                Reset();
                Reserve(Other.Count);
                for (int32 Index = 0; Index < Other.Count; ++Index)
                {
                    PushLast(Other[Index]);
                }
            }
            return *this;
        }

        TDeque& operator=(TDeque&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseBlock();
                Storage = Other.Storage;
                Cap     = Other.Cap;
                Head    = Other.Head;
                Count   = Other.Count;
                Other.AdoptEmpty();
            }
            return *this;
        }

        NODISCARD FORCEINLINE size_type Num() const noexcept { return Count; }
        NODISCARD FORCEINLINE size_type Max() const noexcept { return Cap; }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Count == 0; }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto operator[](this TSelf&& Self, size_type Index) noexcept -> TLikeSelf<TSelf, T>&
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Self.Count);
            return Self.Storage[Self.SlotFor(Index)];
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto First(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>& { return Self[0]; }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto Last(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>& { return Self[Self.Count - 1]; }

        template <typename TSelf>
        NODISCARD auto begin(this TSelf&& Self) noexcept { return TIterator<std::is_const_v<std::remove_reference_t<TSelf>>>(&Self, 0); }

        template <typename TSelf>
        NODISCARD auto end(this TSelf&& Self) noexcept { return TIterator<std::is_const_v<std::remove_reference_t<TSelf>>>(&Self, Self.Count); }
        NODISCARD const_iterator cbegin() const noexcept { return begin(); }
        NODISCARD const_iterator cend() const noexcept { return end(); }

        void Reserve(size_type InNewCapacity)
        {
            const int32 NewCapacity = static_cast<int32>(Private::CheckCapacity(static_cast<uint32>(InNewCapacity), 0x7FFFFFFFu));
            if (NewCapacity > Cap)
            {
                Reallocate(NewCapacity);
            }
        }

        // Built before growing when full, since an argument may name an element the growth is about to free.
        template <typename... TArgs>
        T& EmplaceLast(TArgs&&... Args)
        {
            if (Count == Cap) [[unlikely]]
            {
                T Staged(std::forward<TArgs>(Args)...);
                GrowIfFull();
                return ConstructBack(std::move(Staged));
            }
            return ConstructBack(std::forward<TArgs>(Args)...);
        }

        template <typename... TArgs>
        T& EmplaceFirst(TArgs&&... Args)
        {
            if (Count == Cap) [[unlikely]]
            {
                T Staged(std::forward<TArgs>(Args)...);
                GrowIfFull();
                return ConstructFront(std::move(Staged));
            }
            return ConstructFront(std::forward<TArgs>(Args)...);
        }

        FORCEINLINE T& PushLast(const T& Value) { return EmplaceLast(Value); }
        FORCEINLINE T& PushLast(T&& Value) { return EmplaceLast(std::move(Value)); }
        FORCEINLINE T& PushFirst(const T& Value) { return EmplaceFirst(Value); }
        FORCEINLINE T& PushFirst(T&& Value) { return EmplaceFirst(std::move(Value)); }

        void PopLast() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            --Count;
            Storage[SlotFor(Count)].~T();
        }

        void PopFirst() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            Storage[Head].~T();
            Head = Head + 1 == Cap ? 0 : Head + 1;
            --Count;
        }

        void Reset() noexcept
        {
            for (int32 Index = 0; Index < Count; ++Index)
            {
                Storage[SlotFor(Index)].~T();
            }
            Count = 0;
            Head = 0;
        }

        void Empty() noexcept { ReleaseBlock(); }

        friend void swap(TDeque& Left, TDeque& Right) noexcept
        {
            std::swap(Left.Storage, Right.Storage);
            std::swap(Left.Cap, Right.Cap);
            std::swap(Left.Head, Right.Head);
            std::swap(Left.Count, Right.Count);
        }

        NODISCARD friend bool operator==(const TDeque& Left, const TDeque& Right)
        {
            if (Left.Count != Right.Count)
            {
                return false;
            }
            for (int32 Index = 0; Index < Left.Count; ++Index)
            {
                if (!(Left[Index] == Right[Index]))
                {
                    return false;
                }
            }
            return true;
        }

    private:

        NODISCARD FORCEINLINE int32 SlotFor(int32 Index) const noexcept
        {
            const int32 Slot = Head + Index;
            return Slot < Cap ? Slot : Slot - Cap;
        }

        void AdoptEmpty() noexcept
        {
            Storage  = nullptr;
            Cap = 0;
            Head    = 0;
            Count    = 0;
        }

        void ReleaseBlock() noexcept
        {
            if (Storage == nullptr)
            {
                return;
            }
            Reset();
            TAllocator::Deallocate(Storage, static_cast<size_t>(Cap) * sizeof(T), alignof(T));
            AdoptEmpty();
        }

        FORCEINLINE void GrowIfFull()
        {
            if (Count == Cap) [[unlikely]]
            {
                Reallocate(static_cast<int32>(Private::CheckCapacity(Cap == 0 ? uint64(8) : static_cast<uint64>(Cap) * 2, 0x7FFFFFFFu)));
            }
        }

        template <typename... TArgs>
        FORCEINLINE T& ConstructBack(TArgs&&... Args)
        {
            T* Slot = Storage + SlotFor(Count);
            Memory::ConstructAt(Slot, std::forward<TArgs>(Args)...);
            ++Count;
            return *Slot;
        }

        template <typename... TArgs>
        FORCEINLINE T& ConstructFront(TArgs&&... Args)
        {
            Head = Head == 0 ? Cap - 1 : Head - 1;
            T* Slot = Storage + Head;
            Memory::ConstructAt(Slot, std::forward<TArgs>(Args)...);
            ++Count;
            return *Slot;
        }

        /** Growth unwraps the ring into a fresh block, so the elements come out contiguous and in order. */
        void Reallocate(int32 NewCapacity)
        {
            T* NewStorage = static_cast<T*>(TAllocator::Allocate(static_cast<size_t>(NewCapacity) * sizeof(T), alignof(T)));

            // The live run is at most two pieces of the ring, so it moves as two bulk relocations.
            const int32 FirstPiece = Count < Cap - Head ? Count : Cap - Head;
            ElementOps::RelocateRange(NewStorage, Storage + Head, FirstPiece);
            ElementOps::RelocateRange(NewStorage + FirstPiece, Storage, Count - FirstPiece);

            if (Storage != nullptr)
            {
                TAllocator::Deallocate(Storage, static_cast<size_t>(Cap) * sizeof(T), alignof(T));
            }

            Storage  = NewStorage;
            Cap = NewCapacity;
            Head    = 0;
        }

        T*     Storage  = nullptr;
        int32 Cap   = 0;
        int32 Head  = 0;
        int32 Count = 0;
    };

}

namespace Lumina
{
    template <typename T>
    using TDeque = Containers::TDeque<T>;

    template <typename T, typename TAllocator>
    struct TIsTriviallyRelocatable<Containers::TDeque<T, TAllocator>>
    {
        static constexpr bool Value = true;
    };
}
