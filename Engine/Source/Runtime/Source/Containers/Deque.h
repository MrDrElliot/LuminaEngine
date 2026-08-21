#pragma once

#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "ElementOps.h"

namespace Lumina::Containers
{
    /** Double-ended queue over one circular buffer, so both ends are O(1) and elements stay in one block. */
    template <typename T, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TDeque
    {
    public:

        using value_type      = T;
        using size_type       = size_t;
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

            TIterator& operator+=(difference_type Offset) noexcept { Index += static_cast<size_t>(Offset); return *this; }
            TIterator& operator-=(difference_type Offset) noexcept { Index -= static_cast<size_t>(Offset); return *this; }

            NODISCARD friend TIterator operator+(TIterator It, difference_type Offset) noexcept { return It += Offset; }
            NODISCARD friend TIterator operator-(TIterator It, difference_type Offset) noexcept { return It -= Offset; }

            NODISCARD friend difference_type operator-(const TIterator& Left, const TIterator& Right) noexcept
            {
                return static_cast<difference_type>(Left.Index) - static_cast<difference_type>(Right.Index);
            }

            NODISCARD friend bool operator==(const TIterator& Left, const TIterator& Right) noexcept
            {
                return Left.Index == Right.Index && Left.Owner == Right.Owner;
            }

        private:

            TIterator(FOwner* InOwner, size_t InIndex) noexcept : Owner(InOwner), Index(InIndex) {}

            FOwner* Owner = nullptr;
            size_t  Index = 0;
        };

        using iterator       = TIterator<false>;
        using const_iterator = TIterator<true>;

        TDeque() noexcept = default;

        explicit TDeque(size_t InitialCapacity) { reserve(InitialCapacity); }

        TDeque(const TDeque& Other)
        {
            reserve(Other.Count);
            for (size_t Index = 0; Index < Other.Count; ++Index)
            {
                push_back(Other[Index]);
            }
        }

        TDeque(TDeque&& Other) noexcept
            : Storage(Other.Storage)
            , Capacity(Other.Capacity)
            , First(Other.First)
            , Count(Other.Count)
        {
            Other.AdoptEmpty();
        }

        ~TDeque() { ReleaseBlock(); }

        TDeque& operator=(const TDeque& Other)
        {
            if (this != &Other)
            {
                clear();
                reserve(Other.Count);
                for (size_t Index = 0; Index < Other.Count; ++Index)
                {
                    push_back(Other[Index]);
                }
            }
            return *this;
        }

        TDeque& operator=(TDeque&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseBlock();
                Storage  = Other.Storage;
                Capacity = Other.Capacity;
                First    = Other.First;
                Count    = Other.Count;
                Other.AdoptEmpty();
            }
            return *this;
        }

        NODISCARD FORCEINLINE size_t size() const noexcept { return Count; }
        NODISCARD FORCEINLINE size_t capacity() const noexcept { return Capacity; }
        NODISCARD FORCEINLINE bool empty() const noexcept { return Count == 0; }
        NODISCARD FORCEINLINE size_t Num() const noexcept { return Count; }

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

        NODISCARD FORCEINLINE T& at(size_t Index) noexcept { return (*this)[Index]; }
        NODISCARD FORCEINLINE const T& at(size_t Index) const noexcept { return (*this)[Index]; }

        NODISCARD FORCEINLINE T& front() noexcept { return (*this)[0]; }
        NODISCARD FORCEINLINE const T& front() const noexcept { return (*this)[0]; }
        NODISCARD FORCEINLINE T& back() noexcept { return (*this)[Count - 1]; }
        NODISCARD FORCEINLINE const T& back() const noexcept { return (*this)[Count - 1]; }

        NODISCARD iterator begin() noexcept { return iterator(this, 0); }
        NODISCARD const_iterator begin() const noexcept { return const_iterator(this, 0); }
        NODISCARD iterator end() noexcept { return iterator(this, Count); }
        NODISCARD const_iterator end() const noexcept { return const_iterator(this, Count); }
        NODISCARD const_iterator cbegin() const noexcept { return begin(); }
        NODISCARD const_iterator cend() const noexcept { return end(); }

        void reserve(size_t NewCapacity)
        {
            if (NewCapacity > Capacity)
            {
                Reallocate(NewCapacity);
            }
        }

        template <typename... TArgs>
        T& emplace_back(TArgs&&... Args)
        {
            GrowIfFull();
            T* Slot = Storage + SlotFor(Count);
            ::new (static_cast<void*>(Slot)) T(std::forward<TArgs>(Args)...);
            ++Count;
            return *Slot;
        }

        template <typename... TArgs>
        T& emplace_front(TArgs&&... Args)
        {
            GrowIfFull();
            First = First == 0 ? Capacity - 1 : First - 1;
            T* Slot = Storage + First;
            ::new (static_cast<void*>(Slot)) T(std::forward<TArgs>(Args)...);
            ++Count;
            return *Slot;
        }

        FORCEINLINE T& push_back(const T& Value) { return emplace_back(Value); }
        FORCEINLINE T& push_back(T&& Value) { return emplace_back(std::move(Value)); }
        FORCEINLINE T& push_front(const T& Value) { return emplace_front(Value); }
        FORCEINLINE T& push_front(T&& Value) { return emplace_front(std::move(Value)); }

        void pop_back() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            --Count;
            Storage[SlotFor(Count)].~T();
        }

        void pop_front() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            Storage[First].~T();
            First = First + 1 == Capacity ? 0 : First + 1;
            --Count;
        }

        void clear() noexcept
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                Storage[SlotFor(Index)].~T();
            }
            Count = 0;
            First = 0;
        }

        void swap(TDeque& Other) noexcept
        {
            std::swap(Storage, Other.Storage);
            std::swap(Capacity, Other.Capacity);
            std::swap(First, Other.First);
            std::swap(Count, Other.Count);
        }

        NODISCARD friend bool operator==(const TDeque& Left, const TDeque& Right)
        {
            if (Left.Count != Right.Count)
            {
                return false;
            }
            for (size_t Index = 0; Index < Left.Count; ++Index)
            {
                if (!(Left[Index] == Right[Index]))
                {
                    return false;
                }
            }
            return true;
        }

    private:

        NODISCARD FORCEINLINE size_t SlotFor(size_t Index) const noexcept
        {
            const size_t Slot = First + Index;
            return Slot < Capacity ? Slot : Slot - Capacity;
        }

        void AdoptEmpty() noexcept
        {
            Storage  = nullptr;
            Capacity = 0;
            First    = 0;
            Count    = 0;
        }

        void ReleaseBlock() noexcept
        {
            if (Storage == nullptr)
            {
                return;
            }
            clear();
            TAllocator::Deallocate(Storage, Capacity * sizeof(T), alignof(T));
            AdoptEmpty();
        }

        FORCEINLINE void GrowIfFull()
        {
            if (Count == Capacity) [[unlikely]]
            {
                Reallocate(Capacity == 0 ? 8 : Capacity * 2);
            }
        }

        /** Growth unwraps the ring into a fresh block, so the elements come out contiguous and in order. */
        void Reallocate(size_t NewCapacity)
        {
            T* NewStorage = static_cast<T*>(TAllocator::Allocate(NewCapacity * sizeof(T), alignof(T)));

            for (size_t Index = 0; Index < Count; ++Index)
            {
                ElementOps::RelocateRange(NewStorage + Index, Storage + SlotFor(Index), 1);
            }

            if (Storage != nullptr)
            {
                TAllocator::Deallocate(Storage, Capacity * sizeof(T), alignof(T));
            }

            Storage  = NewStorage;
            Capacity = NewCapacity;
            First    = 0;
        }

        T*     Storage  = nullptr;
        size_t Capacity = 0;
        size_t First    = 0;
        size_t Count    = 0;
    };

    template <typename T, ContainerAllocatorType TAllocator>
    FORCEINLINE void swap(TDeque<T, TAllocator>& Left, TDeque<T, TAllocator>& Right) noexcept
    {
        Left.swap(Right);
    }
}

namespace Lumina
{
    template <typename T>
    using TDeque = Containers::TDeque<T>;
}
