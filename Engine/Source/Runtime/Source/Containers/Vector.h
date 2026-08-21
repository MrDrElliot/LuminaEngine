#pragma once

#include <compare>
#include <initializer_list>
#include <iterator>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "ElementOps.h"

namespace Lumina::Containers
{
    namespace Private
    {
        // A heap-only vector must not query T here: a type holding TVector<itself> is still incomplete.
        template <size_t N, typename U>
        struct TNothrowRelocate : std::bool_constant<std::is_nothrow_move_constructible_v<U>> {};

        template <typename U>
        struct TNothrowRelocate<0, U> : std::true_type {};

        template <typename T, size_t N>
        struct TInlineStorage
        {
            NODISCARD FORCEINLINE T* GetInlineData() { return reinterpret_cast<T*>(Buffer); }
            NODISCARD FORCEINLINE const T* GetInlineData() const { return reinterpret_cast<const T*>(Buffer); }

            alignas(T) uint8 Buffer[N * sizeof(T)];
        };

        // Empty base, so the empty-base optimization keeps TVector<T> at one pointer plus two uint32.
        template <typename T>
        struct TInlineStorage<T, 0>
        {
            NODISCARD FORCEINLINE T* GetInlineData() { return nullptr; }
            NODISCARD FORCEINLINE const T* GetInlineData() const { return nullptr; }
        };
    }

    /** Contiguous dynamic array; size and capacity are stored as uint32 but reported as size_t. */
    template <typename T, size_t InlineCapacity = 0, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TVector : private Private::TInlineStorage<T, InlineCapacity>
    {
        using FStorage = Private::TInlineStorage<T, InlineCapacity>;

    public:

        using value_type             = T;
        using size_type              = size_t;
        using difference_type        = ptrdiff_t;
        using reference              = T&;
        using const_reference        = const T&;
        using pointer                = T*;
        using const_pointer          = const T*;
        using iterator               = T*;
        using const_iterator         = const T*;
        using reverse_iterator       = std::reverse_iterator<T*>;
        using const_reverse_iterator = std::reverse_iterator<const T*>;
        using allocator_type         = TAllocator;

        static constexpr size_t npos            = ~static_cast<size_t>(0);
        static constexpr size_t MaxCapacity     = 0xFFFFFFFFu;
        static constexpr size_t InlineCapacityV = InlineCapacity;

        TVector() noexcept
        {
            AdoptInlineStorage();
        }

        explicit TVector(size_t InCount)
        {
            AdoptInlineStorage();
            resize(InCount);
        }

        TVector(size_t InCount, const T& Value)
        {
            AdoptInlineStorage();
            resize(InCount, Value);
        }

        TVector(std::initializer_list<T> Init)
        {
            AdoptInlineStorage();
            assign(Init.begin(), Init.end());
        }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TVector(TIter First, TIter Last)
        {
            AdoptInlineStorage();
            assign(First, Last);
        }

        TVector(const TVector& Other)
        {
            AdoptInlineStorage();
            Reserve(Other.Count);
            ElementOps::CopyConstructRange(Data, Other.Data, Other.Count);
            Count = Other.Count;
        }

        TVector(TVector&& Other) noexcept(Private::TNothrowRelocate<InlineCapacity, T>::value)
        {
            AdoptInlineStorage();

            if (Other.OwnsHeapBlock())
            {
                Data  = Other.Data;
                Count = Other.Count;
                Cap   = Other.Cap;
                Other.ResetToEmpty();
            }
            else
            {
                Reserve(Other.Count);
                ElementOps::RelocateRange(Data, Other.Data, Other.Count);
                Count = Other.Count;
                Other.Count = 0;
            }
        }

        ~TVector()
        {
            ElementOps::DestructRange(Data, Count);
            ReleaseHeapBlock();
        }

        TVector& operator=(const TVector& Other)
        {
            if (this != &Other)
            {
                assign(Other.Data, Other.Data + Other.Count);
            }
            return *this;
        }

        TVector& operator=(TVector&& Other) noexcept(Private::TNothrowRelocate<InlineCapacity, T>::value)
        {
            if (this == &Other)
            {
                return *this;
            }

            ElementOps::DestructRange(Data, Count);
            Count = 0;

            if (Other.OwnsHeapBlock())
            {
                ReleaseHeapBlock();
                Data  = Other.Data;
                Count = Other.Count;
                Cap   = Other.Cap;
                Other.ResetToEmpty();
            }
            else
            {
                Reserve(Other.Count);
                ElementOps::RelocateRange(Data, Other.Data, Other.Count);
                Count = Other.Count;
                Other.Count = 0;
            }
            return *this;
        }

        TVector& operator=(std::initializer_list<T> Init)
        {
            assign(Init.begin(), Init.end());
            return *this;
        }

        NODISCARD FORCEINLINE T* data() noexcept { return Data; }
        NODISCARD FORCEINLINE const T* data() const noexcept { return Data; }

        NODISCARD FORCEINLINE size_t size() const noexcept { return Count; }
        NODISCARD FORCEINLINE size_t capacity() const noexcept { return Cap; }
        NODISCARD FORCEINLINE bool empty() const noexcept { return Count == 0; }
        NODISCARD FORCEINLINE size_t size_bytes() const noexcept { return static_cast<size_t>(Count) * sizeof(T); }
        NODISCARD static constexpr size_t max_size() noexcept { return MaxCapacity; }

        /** True while the elements live in the inline buffer rather than a heap block. */
        NODISCARD FORCEINLINE bool IsInline() const noexcept
        {
            if constexpr (InlineCapacity == 0)
            {
                return false;
            }
            else
            {
                return Data == FStorage::GetInlineData();
            }
        }

        NODISCARD FORCEINLINE T* begin() noexcept { return Data; }
        NODISCARD FORCEINLINE const T* begin() const noexcept { return Data; }
        NODISCARD FORCEINLINE const T* cbegin() const noexcept { return Data; }
        NODISCARD FORCEINLINE T* end() noexcept { return Data + Count; }
        NODISCARD FORCEINLINE const T* end() const noexcept { return Data + Count; }
        NODISCARD FORCEINLINE const T* cend() const noexcept { return Data + Count; }

        NODISCARD FORCEINLINE reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        NODISCARD FORCEINLINE const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        NODISCARD FORCEINLINE reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        NODISCARD FORCEINLINE const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

        NODISCARD FORCEINLINE T& operator[](size_t Index) noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Count);
            return Data[Index];
        }

        NODISCARD FORCEINLINE const T& operator[](size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Count);
            return Data[Index];
        }

        NODISCARD FORCEINLINE T& at(size_t Index) noexcept { return (*this)[Index]; }
        NODISCARD FORCEINLINE const T& at(size_t Index) const noexcept { return (*this)[Index]; }

        NODISCARD FORCEINLINE T& front() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return Data[0];
        }

        NODISCARD FORCEINLINE const T& front() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return Data[0];
        }

        NODISCARD FORCEINLINE T& back() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return Data[Count - 1];
        }

        NODISCARD FORCEINLINE const T& back() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return Data[Count - 1];
        }

        void Reserve(size_t NewCapacity)
        {
            if (NewCapacity > Cap)
            {
                ReallocateTo(ToCapacity(NewCapacity));
            }
        }

        FORCEINLINE void reserve(size_t NewCapacity) { Reserve(NewCapacity); }

        // Reserve honors the request exactly; the growth paths go through here so they stay amortized.
        FORCEINLINE void ReserveForGrowth(size_t Required)
        {
            if (Required > Cap)
            {
                ReallocateTo(CalculateGrowth(Cap, Required));
            }
        }

        void shrink_to_fit()
        {
            if (Count == Cap || IsInline())
            {
                return;
            }

            if constexpr (InlineCapacity > 0)
            {
                if (Count <= InlineCapacity)
                {
                    T* Inline = FStorage::GetInlineData();
                    ElementOps::RelocateRange(Inline, Data, Count);
                    ReleaseHeapBlock();
                    Data = Inline;
                    Cap  = static_cast<uint32>(InlineCapacity);
                    return;
                }
            }

            if (Count == 0)
            {
                ReleaseHeapBlock();
                ResetToEmpty();
                return;
            }

            ReallocateTo(Count);
        }

        FORCEINLINE void clear() noexcept
        {
            ElementOps::DestructRange(Data, Count);
            Count = 0;
        }

        /** Drops the elements and returns the capacity to the allocator, unlike clear which keeps it. */
        void Reset()
        {
            ElementOps::DestructRange(Data, Count);
            ReleaseHeapBlock();
            ResetToEmpty();
        }

        void resize(size_t NewCount)
        {
            if (NewCount < Count)
            {
                ElementOps::DestructRange(Data + NewCount, Count - NewCount);
            }
            else if (NewCount > Count)
            {
                ReserveForGrowth(NewCount);
                ElementOps::DefaultConstructRange(Data + Count, NewCount - Count);
            }
            Count = static_cast<uint32>(NewCount);
        }

        void resize(size_t NewCount, const T& Value)
        {
            if (NewCount < Count)
            {
                ElementOps::DestructRange(Data + NewCount, Count - NewCount);
            }
            else if (NewCount > Count)
            {
                ReserveForGrowth(NewCount);
                ElementOps::FillConstructRange(Data + Count, NewCount - Count, Value);
            }
            Count = static_cast<uint32>(NewCount);
        }

        /** Grows by Extra elements without constructing them; the caller must construct every slot. */
        NODISCARD T* AddUninitialized(size_t Extra)
        {
            ReserveForGrowth(static_cast<size_t>(Count) + Extra);
            T* Slot = Data + Count;
            Count += static_cast<uint32>(Extra);
            return Slot;
        }

        /** Grows by one default-constructed element and hands back a reference to it. */
        FORCEINLINE T& push_back()
        {
            return emplace_back();
        }

        FORCEINLINE T& push_back(const T& Value)
        {
            if (Count == Cap) [[unlikely]]
            {
                return EmplaceBackGrowing(Value);
            }

            T* Slot = Data + Count;
            ::new (static_cast<void*>(Slot)) T(Value);
            ++Count;
            return *Slot;
        }

        FORCEINLINE T& push_back(T&& Value)
        {
            if (Count == Cap) [[unlikely]]
            {
                return EmplaceBackGrowing(std::move(Value));
            }

            T* Slot = Data + Count;
            ::new (static_cast<void*>(Slot)) T(std::move(Value));
            ++Count;
            return *Slot;
        }

        template <typename... TArgs>
        FORCEINLINE T& emplace_back(TArgs&&... Args)
        {
            if (Count == Cap) [[unlikely]]
            {
                return EmplaceBackGrowing(std::forward<TArgs>(Args)...);
            }

            T* Slot = Data + Count;
            ::new (static_cast<void*>(Slot)) T(std::forward<TArgs>(Args)...);
            ++Count;
            return *Slot;
        }

        FORCEINLINE void pop_back() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            --Count;
            ElementOps::DestructRange(Data + Count, 1);
        }

        /** pop_back that hands the element back, so a work queue can drain without a copy. */
        NODISCARD T PopBackValue()
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            --Count;
            T Result = std::move(Data[Count]);
            ElementOps::DestructRange(Data + Count, 1);
            return Result;
        }

        T* insert(const T* Pos, const T& Value)
        {
            const size_t Index = ValidateInsertPosition(Pos);

            if (IsOwnedAddress(&Value))
            {
                T Local(Value);
                return InsertAtIndex(Index, std::move(Local));
            }
            return InsertAtIndex(Index, Value);
        }

        T* insert(const T* Pos, T&& Value)
        {
            const size_t Index = ValidateInsertPosition(Pos);

            if (IsOwnedAddress(&Value))
            {
                T Local(std::move(Value));
                return InsertAtIndex(Index, std::move(Local));
            }
            return InsertAtIndex(Index, std::move(Value));
        }

        template <typename... TArgs>
        T* emplace(const T* Pos, TArgs&&... Args)
        {
            const size_t Index = ValidateInsertPosition(Pos);
            T Local(std::forward<TArgs>(Args)...);
            return InsertAtIndex(Index, std::move(Local));
        }

        T* insert(const T* Pos, size_t Repeat, const T& Value)
        {
            const size_t Index = ValidateInsertPosition(Pos);
            if (Repeat == 0)
            {
                return Data + Index;
            }

            T Local(Value);
            OpenGap(Index, Repeat);
            ElementOps::FillConstructRange(Data + Index, Repeat, Local);
            Count += static_cast<uint32>(Repeat);
            return Data + Index;
        }

        template <typename TIter>
        requires std::input_iterator<TIter>
        T* insert(const T* Pos, TIter First, TIter Last)
        {
            const size_t Index = ValidateInsertPosition(Pos);

            if constexpr (std::forward_iterator<TIter>)
            {
                const size_t Extra = static_cast<size_t>(std::distance(First, Last));
                if (Extra == 0)
                {
                    return Data + Index;
                }

                OpenGap(Index, Extra);
                T* Write = Data + Index;
                for (; First != Last; ++First, ++Write)
                {
                    ::new (static_cast<void*>(Write)) T(*First);
                }
                Count += static_cast<uint32>(Extra);
                return Data + Index;
            }
            else
            {
                TVector Staged(First, Last);
                return insert(Data + Index, Staged.begin(), Staged.end());
            }
        }

        T* insert(const T* Pos, std::initializer_list<T> Init)
        {
            return insert(Pos, Init.begin(), Init.end());
        }

        T* erase(const T* Pos)
        {
            const size_t Index = static_cast<size_t>(Pos - Data);
            LUMINA_CONTAINER_CHECK_INDEX(Index, Count);

            ElementOps::DestructRange(Data + Index, 1);
            ElementOps::RelocateRangeOverlapping(Data + Index, Data + Index + 1, Count - Index - 1);
            --Count;
            return Data + Index;
        }

        T* erase(const T* First, const T* Last)
        {
            const size_t Index   = static_cast<size_t>(First - Data);
            const size_t Removed = static_cast<size_t>(Last - First);
            LUMINA_CONTAINER_CHECK_WITHIN(Index, Count);
            LUMINA_CONTAINER_CHECK_WITHIN(Index + Removed, Count);

            if (Removed == 0)
            {
                return Data + Index;
            }

            ElementOps::DestructRange(Data + Index, Removed);
            ElementOps::RelocateRangeOverlapping(Data + Index, Data + Index + Removed, Count - Index - Removed);
            Count -= static_cast<uint32>(Removed);
            return Data + Index;
        }

        /** Erase in constant time by moving the last element into the hole; does not preserve order. */
        void RemoveAtSwap(size_t Index)
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Count);

            ElementOps::DestructRange(Data + Index, 1);
            --Count;
            if (Index != Count)
            {
                ElementOps::RelocateRange(Data + Index, Data + Count, 1);
            }
        }

        void EraseAt(size_t Index)
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Count);
            erase(Data + Index);
        }

        NODISCARD size_t IndexOf(const T& Value) const
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                if (Data[Index] == Value)
                {
                    return Index;
                }
            }
            return npos;
        }

        template <typename TPredicate>
        NODISCARD size_t IndexOfBy(TPredicate&& Predicate) const
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                if (Predicate(Data[Index]))
                {
                    return Index;
                }
            }
            return npos;
        }

        NODISCARD FORCEINLINE bool Contains(const T& Value) const { return IndexOf(Value) != npos; }

        NODISCARD T* Find(const T& Value)
        {
            const size_t Index = IndexOf(Value);
            return Index == npos ? end() : Data + Index;
        }

        NODISCARD const T* Find(const T& Value) const
        {
            const size_t Index = IndexOf(Value);
            return Index == npos ? end() : Data + Index;
        }

        template <typename TPredicate>
        NODISCARD T* FindBy(TPredicate&& Predicate)
        {
            const size_t Index = IndexOfBy(std::forward<TPredicate>(Predicate));
            return Index == npos ? end() : Data + Index;
        }

        template <typename TPredicate>
        NODISCARD const T* FindBy(TPredicate&& Predicate) const
        {
            const size_t Index = IndexOfBy(std::forward<TPredicate>(Predicate));
            return Index == npos ? end() : Data + Index;
        }

        /** Appends only if absent; returns the index of the element either way. */
        size_t AddUnique(const T& Value)
        {
            const size_t Existing = IndexOf(Value);
            if (Existing != npos)
            {
                return Existing;
            }
            push_back(Value);
            return Count - 1;
        }

        bool RemoveFirst(const T& Value)
        {
            const size_t Index = IndexOf(Value);
            if (Index == npos)
            {
                return false;
            }
            EraseAt(Index);
            return true;
        }

        bool RemoveFirstSwap(const T& Value)
        {
            const size_t Index = IndexOf(Value);
            if (Index == npos)
            {
                return false;
            }
            RemoveAtSwap(Index);
            return true;
        }

        /** Erase-remove in one pass; returns how many elements went away. */
        template <typename TPredicate>
        size_t RemoveAllBy(TPredicate&& Predicate)
        {
            size_t Write = 0;
            for (size_t Read = 0; Read < Count; ++Read)
            {
                if (Predicate(Data[Read]))
                {
                    ElementOps::DestructRange(Data + Read, 1);
                    continue;
                }

                if (Write != Read)
                {
                    ElementOps::RelocateRange(Data + Write, Data + Read, 1);
                }
                ++Write;
            }

            const size_t Removed = Count - Write;
            Count = static_cast<uint32>(Write);
            return Removed;
        }

        size_t RemoveAll(const T& Value)
        {
            return RemoveAllBy([&Value](const T& Element) { return Element == Value; });
        }

        template <typename TIter>
        requires std::input_iterator<TIter>
        void assign(TIter First, TIter Last)
        {
            clear();

            if constexpr (std::forward_iterator<TIter>)
            {
                const size_t Incoming = static_cast<size_t>(std::distance(First, Last));
                Reserve(Incoming);

                if constexpr (std::contiguous_iterator<TIter> && std::is_same_v<std::iter_value_t<TIter>, T>)
                {
                    ElementOps::CopyConstructRange(Data, std::to_address(First), Incoming);
                    Count = static_cast<uint32>(Incoming);
                    return;
                }
                else
                {
                    T* Write = Data;
                    for (; First != Last; ++First, ++Write)
                    {
                        ::new (static_cast<void*>(Write)) T(*First);
                    }
                    Count = static_cast<uint32>(Incoming);
                }
            }
            else
            {
                for (; First != Last; ++First)
                {
                    push_back(*First);
                }
            }
        }

        void assign(size_t Repeat, const T& Value)
        {
            clear();
            resize(Repeat, Value);
        }

        void assign(std::initializer_list<T> Init) { assign(Init.begin(), Init.end()); }

        template <typename TIter>
        requires std::input_iterator<TIter>
        void Append(TIter First, TIter Last)
        {
            insert(end(), First, Last);
        }

        void Append(const TVector& Other) { insert(end(), Other.begin(), Other.end()); }

        void Append(std::initializer_list<T> Init) { insert(end(), Init.begin(), Init.end()); }

        void swap(TVector& Other) noexcept
        {
            if (this == &Other)
            {
                return;
            }

            if constexpr (InlineCapacity == 0)
            {
                T* const     SwapData  = Data;
                const uint32 SwapCount = Count;
                const uint32 SwapCap   = Cap;

                Data  = Other.Data;
                Count = Other.Count;
                Cap   = Other.Cap;

                Other.Data  = SwapData;
                Other.Count = SwapCount;
                Other.Cap   = SwapCap;
            }
            else
            {
                TVector Staged(std::move(*this));
                *this = std::move(Other);
                Other = std::move(Staged);
            }
        }

        NODISCARD friend bool operator==(const TVector& A, const TVector& B)
        {
            return A.Count == B.Count && ElementOps::RangeEquals(A.Data, B.Data, A.Count);
        }

        NODISCARD friend auto operator<=>(const TVector& A, const TVector& B)
        {
            return std::lexicographical_compare_three_way(A.begin(), A.end(), B.begin(), B.end());
        }

    private:

        FORCEINLINE void AdoptInlineStorage() noexcept
        {
            if constexpr (InlineCapacity > 0)
            {
                Data = FStorage::GetInlineData();
                Cap  = static_cast<uint32>(InlineCapacity);
            }
        }

        FORCEINLINE void ResetToEmpty() noexcept
        {
            Data  = FStorage::GetInlineData();
            Count = 0;
            Cap   = static_cast<uint32>(InlineCapacity);
        }

        NODISCARD FORCEINLINE bool OwnsHeapBlock() const noexcept
        {
            return Data != nullptr && !IsInline();
        }

        FORCEINLINE void ReleaseHeapBlock() noexcept
        {
            if (OwnsHeapBlock())
            {
                TAllocator::Deallocate(Data, static_cast<size_t>(Cap) * sizeof(T), alignof(T));
            }
        }

        NODISCARD FORCEINLINE bool IsOwnedAddress(const T* Address) const noexcept
        {
            return Address >= Data && Address < Data + Count;
        }

        NODISCARD static uint32 ToCapacity(size_t Requested)
        {
            LUMINA_CONTAINER_CHECK_WITHIN(Requested, MaxCapacity);
            return static_cast<uint32>(Requested);
        }

        // Measured against 1.5x and a hybrid: doubling wins, because 1.5x copies half again as many bytes.
        NODISCARD static uint32 CalculateGrowth(uint32 CurrentCapacity, size_t Required)
        {
            size_t Grown = (CurrentCapacity < 4) ? 4 : static_cast<size_t>(CurrentCapacity) * 2;

            if (Grown < Required)
            {
                Grown = Required;
            }
            return ToCapacity(Grown);
        }

        void ReallocateTo(uint32 NewCapacity)
        {
            LUMINA_CONTAINER_CHECK(NewCapacity >= Count);

            const size_t NewBytes = static_cast<size_t>(NewCapacity) * sizeof(T);

            if (NewCapacity > Cap && OwnsHeapBlock())
            {
                if (TAllocator::TryExpand(Data, static_cast<size_t>(Cap) * sizeof(T), NewBytes))
                {
                    Cap = NewCapacity;
                    return;
                }
            }

            T* NewData = static_cast<T*>(TAllocator::Allocate(NewBytes, alignof(T)));
            ElementOps::RelocateRange(NewData, Data, Count);
            ReleaseHeapBlock();

            Data = NewData;
            Cap  = NewCapacity;
        }

        // Constructs into the new block before relocating, so an argument aliasing our own buffer stays live.
        template <typename... TArgs>
        T& EmplaceBackGrowing(TArgs&&... Args)
        {
            const uint32 NewCapacity = CalculateGrowth(Cap, static_cast<size_t>(Cap) + 1);
            const size_t NewBytes    = static_cast<size_t>(NewCapacity) * sizeof(T);

            if (OwnsHeapBlock() && TAllocator::TryExpand(Data, static_cast<size_t>(Cap) * sizeof(T), NewBytes))
            {
                Cap = NewCapacity;
                T* Slot = Data + Count;
                ::new (static_cast<void*>(Slot)) T(std::forward<TArgs>(Args)...);
                ++Count;
                return *Slot;
            }

            T* NewData = static_cast<T*>(TAllocator::Allocate(NewBytes, alignof(T)));
            T* Slot    = NewData + Count;
            ::new (static_cast<void*>(Slot)) T(std::forward<TArgs>(Args)...);

            ElementOps::RelocateRange(NewData, Data, Count);
            ReleaseHeapBlock();

            Data = NewData;
            Cap  = NewCapacity;
            ++Count;
            return *Slot;
        }

        NODISCARD size_t ValidateInsertPosition(const T* Pos) const noexcept
        {
            const size_t Index = static_cast<size_t>(Pos - Data);
            LUMINA_CONTAINER_CHECK_WITHIN(Index, Count);
            return Index;
        }

        // Leaves Extra raw slots at Index for the caller to construct into; Count is not updated here.
        void OpenGap(size_t Index, size_t Extra)
        {
            ReserveForGrowth(static_cast<size_t>(Count) + Extra);
            ElementOps::RelocateRangeOverlapping(Data + Index + Extra, Data + Index, Count - Index);
        }

        template <typename TValue>
        T* InsertAtIndex(size_t Index, TValue&& Value)
        {
            OpenGap(Index, 1);
            ::new (static_cast<void*>(Data + Index)) T(std::forward<TValue>(Value));
            ++Count;
            return Data + Index;
        }

        T*     Data  = nullptr;
        uint32 Count = 0;
        uint32 Cap   = 0;
    };

    template <typename T, size_t N, ContainerAllocatorType TAllocator, typename TPredicate>
    size_t erase_if(TVector<T, N, TAllocator>& Values, TPredicate Predicate)
    {
        const size_t Before = Values.size();
        size_t Write = 0;
        for (size_t Read = 0; Read < Values.size(); ++Read)
        {
            if (!Predicate(Values[Read]))
            {
                if (Write != Read)
                {
                    Values[Write] = std::move(Values[Read]);
                }
                ++Write;
            }
        }
        Values.resize(Write);
        return Before - Write;
    }

    template <typename T, size_t N, ContainerAllocatorType TAllocator, typename TValue>
    size_t erase(TVector<T, N, TAllocator>& Values, const TValue& Value)
    {
        return erase_if(Values, [&Value](const T& Element) { return Element == Value; });
    }

    template <typename T, size_t N, typename TAllocator>
    FORCEINLINE void swap(TVector<T, N, TAllocator>& A, TVector<T, N, TAllocator>& B) noexcept
    {
        A.swap(B);
    }

    template <typename T, size_t N>
    using TInlineVector = TVector<T, N>;

    template <typename T>
    using TScratchVector = TVector<T, 0, FScratchAllocator>;
}

namespace Lumina
{
    // Only the no-inline-capacity form; an inline buffer makes the vector point into itself.
    template <typename T, typename TAllocator>
    struct TIsTriviallyRelocatable<Containers::TVector<T, 0, TAllocator>>
    {
        static constexpr bool Value = true;
    };
}

namespace Lumina
{
    template <typename T, size_t InlineCapacity = 0, ContainerAllocatorType TAllocator = FHeapAllocator>
    using TVector = Containers::TVector<T, InlineCapacity, TAllocator>;

    template <typename T, size_t S, bool bOverflow = true>
    using TFixedVector = Containers::TVector<T, S>;
}
