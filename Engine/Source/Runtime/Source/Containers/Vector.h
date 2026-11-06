#pragma once

#include <compare>
#include <initializer_list>
#include <iterator>

#include "ContainerAllocator.h"
#include "Core/Templates/NumericLimits.h"
#include "ContainerTraits.h"
#include "ElementOps.h"
#include "Memory/Construct.h"

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

        // Empty base, so the empty-base optimization keeps TVector<T> at one pointer plus two int32.
        template <typename T>
        struct TInlineStorage<T, 0>
        {
            NODISCARD FORCEINLINE T* GetInlineData() { return nullptr; }
            NODISCARD FORCEINLINE const T* GetInlineData() const { return nullptr; }
        };
    }

    /** Contiguous dynamic array shaped after Unreal's TArray, counted in its allocator's SizeType (SizeType by default). */
    template <typename T, size_t InlineCapacity = 0, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TVector : private Private::TInlineStorage<T, InlineCapacity>
    {
        using FStorage = Private::TInlineStorage<T, InlineCapacity>;

    public:

        using ElementType            = T;
        using SizeType               = typename TAllocatorSizeType<TAllocator>::Type;
        using USizeType              = std::make_unsigned_t<SizeType>;
        using value_type             = T;
        using size_type              = SizeType;
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

        static constexpr SizeType MaxElements  = TNumericLimits<SizeType>::Max();
        static constexpr SizeType InlineCapacityV = static_cast<SizeType>(InlineCapacity);

        // Constant-initializable, so a global vector can be constinit and no static initializer can run before it.
        constexpr TVector() noexcept
        {
            AdoptInlineStorage();
        }

        explicit TVector(SizeType InNum)
        {
            AdoptInlineStorage();
            SetNum(InNum);
        }

        TVector(SizeType InNum, const T& Value)
        {
            AdoptInlineStorage();
            SetNum(InNum, Value);
        }

        TVector(const T* Ptr, SizeType Count)
        {
            AdoptInlineStorage();
            ReserveExact(static_cast<USizeType>(Count));
            Append(Ptr, Count);
        }

        TVector(std::initializer_list<T> Init)
        {
            AdoptInlineStorage();
            ReserveExact(static_cast<USizeType>(Init.size()));
            Append(Init.begin(), static_cast<SizeType>(Init.size()));
        }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TVector(TIter First, TIter Last)
        {
            AdoptInlineStorage();
            AppendRange(First, Last);
        }

        TVector(const TVector& Other)
        {
            AdoptInlineStorage();
            ReserveExact(static_cast<USizeType>(Other.ArrayNum));
            ElementOps::CopyConstructRange(Elements, Other.Elements, Other.ArrayNum);
            ArrayNum = Other.ArrayNum;
        }

        TVector(TVector&& Other) noexcept(Private::TNothrowRelocate<InlineCapacity, T>::value)
        {
            AdoptInlineStorage();
            TakeFrom(Other);
        }

        // Both heap allocators share one heap, so a vector of the other width hands over its block without a copy.
        template <ContainerAllocatorType TOtherAllocator>
        requires (InlineCapacity == 0 && !std::is_same_v<TOtherAllocator, TAllocator>
            && std::is_base_of_v<FHeapAllocator, TAllocator> && std::is_base_of_v<FHeapAllocator, TOtherAllocator>)
        explicit(sizeof(typename TAllocatorSizeType<TOtherAllocator>::Type) > sizeof(SizeType)) TVector(TVector<T, 0, TOtherAllocator>&& Other)
        {
            AdoptInlineStorage();
            if (static_cast<uint64>(Other.ArrayMax) > static_cast<uint64>(MaxElements))
            {
                Other.Shrink();
            }
            const SizeType TakenNum = CheckedCount(static_cast<uint64>(Other.ArrayNum));
            const SizeType TakenMax = CheckedCount(static_cast<uint64>(Other.ArrayMax));
            Elements = Other.Elements;
            ArrayNum = TakenNum;
            ArrayMax = TakenMax;
            Other.ResetToEmpty();
        }

        ~TVector()
        {
            ElementOps::DestructRange(Elements, ArrayNum);
            ReleaseHeapBlock();
        }

        // Through a copy when Other lives inside one of our elements, which assigning would destroy first.
        TVector& operator=(const TVector& Other)
        {
            if (this != &Other)
            {
                if (IsOwnedRegion(&Other))
                {
                    TVector Copy(Other);
                    *this = std::move(Copy);
                }
                else
                {
                    // Assigned over within capacity, so each element keeps its storage; one that cannot be assigned is rebuilt.
                    if constexpr (std::is_copy_assignable_v<T>)
                    {
                        if (Other.ArrayNum <= ArrayMax)
                        {
                            const USizeType Common = static_cast<USizeType>(ArrayNum < Other.ArrayNum ? ArrayNum : Other.ArrayNum);
                            ElementOps::CopyAssignRange(Elements, Other.Elements, Common);
                            if (Other.ArrayNum > ArrayNum)
                            {
                                ElementOps::CopyConstructRange(Elements + ArrayNum, Other.Elements + ArrayNum, Other.ArrayNum - ArrayNum);
                            }
                            else
                            {
                                ElementOps::DestructRange(Elements + Other.ArrayNum, ArrayNum - Other.ArrayNum);
                            }
                            ArrayNum = Other.ArrayNum;
                            return *this;
                        }
                    }
                    Reset();
                    ReserveExact(static_cast<USizeType>(Other.ArrayNum));
                    Append(Other.Elements, Other.ArrayNum);
                }
            }
            return *this;
        }

        // Other is emptied before our elements go, since it may live inside one of them.
        TVector& operator=(TVector&& Other) noexcept(Private::TNothrowRelocate<InlineCapacity, T>::value)
        {
            if (this != &Other)
            {
                TVector Taken(std::move(Other));
                ElementOps::DestructRange(Elements, ArrayNum);
                ArrayNum = 0;
                if (InlineCapacity == 0 || Taken.OwnsHeapBlock())
                {
                    ReleaseHeapBlock();
                    ResetToEmpty();
                }
                TakeFrom(Taken);
            }
            return *this;
        }

        TVector& operator=(std::initializer_list<T> Init)
        {
            Reset();
            ReserveExact(static_cast<USizeType>(Init.size()));
            Append(Init.begin(), static_cast<SizeType>(Init.size()));
            return *this;
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto GetData(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>* { return Self.Elements; }

        NODISCARD FORCEINLINE SizeType Num() const noexcept { return ArrayNum; }
        NODISCARD FORCEINLINE SizeType Max() const noexcept { return ArrayMax; }
        NODISCARD FORCEINLINE SizeType GetSlack() const noexcept { return ArrayMax - ArrayNum; }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return ArrayNum == 0; }
        NODISCARD FORCEINLINE bool IsValidIndex(SizeType Index) const noexcept { return static_cast<USizeType>(Index) < static_cast<USizeType>(ArrayNum); }
        NODISCARD FORCEINLINE size_t NumBytes() const noexcept { return static_cast<size_t>(ArrayNum) * sizeof(T); }
        NODISCARD FORCEINLINE size_t GetAllocatedSize() const noexcept { return OwnsHeapBlock() ? static_cast<size_t>(ArrayMax) * sizeof(T) : 0; }

        // C# reads the buffer and count in place, and an inline buffer base moves them off offset zero.
        static uint32 GetDataOffsetForInterop() noexcept { return static_cast<uint32>(offsetof(TVector, Elements)); }
        static uint32 GetCountOffsetForInterop() noexcept { return static_cast<uint32>(offsetof(TVector, ArrayNum)); }

        /** True while the elements live in the inline buffer rather than a heap block. */
        NODISCARD FORCEINLINE bool IsInline() const noexcept
        {
            if constexpr (InlineCapacity == 0)
            {
                return false;
            }
            else
            {
                return Elements == FStorage::GetInlineData();
            }
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto begin(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>* { return Self.Elements; }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto end(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>* { return Self.Elements + Self.ArrayNum; }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto rbegin(this TSelf&& Self) noexcept { return std::reverse_iterator(Self.end()); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto rend(this TSelf&& Self) noexcept { return std::reverse_iterator(Self.begin()); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto operator[](this TSelf&& Self, SizeType Index) noexcept -> TLikeSelf<TSelf, T>&
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, Self.ArrayNum);
            return Self.Elements[Index];
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto Last(this TSelf&& Self, SizeType IndexFromEnd = 0) noexcept -> TLikeSelf<TSelf, T>&
        {
            return Self[Self.ArrayNum - IndexFromEnd - 1];
        }

        FORCEINLINE SizeType Add(const T& Item) { return Emplace(Item); }
        FORCEINLINE SizeType Add(T&& Item) { return Emplace(std::move(Item)); }
        FORCEINLINE T& AddGetRef(const T& Item) { return EmplaceGetRef(Item); }
        FORCEINLINE T& AddGetRef(T&& Item) { return EmplaceGetRef(std::move(Item)); }

        template <typename... TArgs>
        FORCEINLINE SizeType Emplace(TArgs&&... Args)
        {
            EmplaceGetRef(std::forward<TArgs>(Args)...);
            return ArrayNum - 1;
        }

        template <typename... TArgs>
        FORCEINLINE T& EmplaceGetRef(TArgs&&... Args)
        {
            if (ArrayNum == ArrayMax) [[unlikely]]
            {
                // By value, so the caller's temporary is never spilled, where its narrow stores would stall the wide copy-load.
                if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) <= 2 * sizeof(void*))
                {
                    return EmplaceGrowingValue(T(std::forward<TArgs>(Args)...));
                }
                else
                {
                    return EmplaceGrowing(std::forward<TArgs>(Args)...);
                }
            }
            T* Slot = Elements + ArrayNum;
            Memory::ConstructAt(Slot, std::forward<TArgs>(Args)...);
            ++ArrayNum;
            return *Slot;
        }

        SizeType AddUnique(const T& Item)
        {
            const SizeType Existing = Find(Item);
            return Existing != Constants::kIndexNone ? Existing : Add(Item);
        }

        /** Grows by Count raw slots and returns the first index; the caller constructs every one of them. */
        SizeType AddUninitialized(SizeType Count = 1)
        {
            const SizeType OldNum = ArrayNum;
            ReserveForGrowth(RequiredFor(Count));
            ArrayNum += Count;
            return OldNum;
        }

        SizeType AddDefaulted(SizeType Count = 1)
        {
            const SizeType Index = AddUninitialized(Count);
            ElementOps::DefaultConstructRange(Elements + Index, Count);
            return Index;
        }

        T& AddDefaultedGetRef()
        {
            // Indexed in two steps, since the array operand is read before the subscript grows the storage.
            const SizeType Index = AddDefaulted(1);
            return Elements[Index];
        }

        SizeType AddZeroed(SizeType Count = 1)
        {
            static_assert(std::is_trivially_default_constructible_v<T>, "AddZeroed needs a T whose zero bytes are a valid value");
            const SizeType Index = AddUninitialized(Count);
            Memory::Memzero(static_cast<void*>(Elements + Index), static_cast<size_t>(Count) * sizeof(T));
            return Index;
        }

        void Append(const T* Ptr, SizeType Count)
        {
            Insert(Ptr, Count, ArrayNum);
        }

        void Append(const TVector& Other) { Insert(Other.Elements, Other.ArrayNum, ArrayNum); }

        // Taken out first when Other lives inside one of our elements, since growing would move it.
        void Append(TVector&& Other)
        {
            if (&Other == this)
            {
                Append(static_cast<const TVector&>(*this));
                return;
            }
            if (IsOwnedRegion(&Other))
            {
                TVector Local(std::move(Other));
                Append(std::move(Local));
                return;
            }
            if (IsEmpty())
            {
                *this = std::move(Other);
                return;
            }
            const SizeType Index = AddUninitialized(Other.ArrayNum);
            ElementOps::RelocateRange(Elements + Index, Other.Elements, Other.ArrayNum);
            Other.ArrayNum = 0;
        }

        void Append(std::initializer_list<T> Init) { Insert(Init.begin(), static_cast<SizeType>(Init.size()), ArrayNum); }

        template <typename TIter>
        requires std::input_iterator<TIter>
        void Append(TIter First, TIter Last)
        {
            AppendRange(First, Last);
        }

        template <typename TRange>
        requires (!std::is_same_v<std::remove_cvref_t<TRange>, TVector> && requires(TRange& Range) { Range.begin(); Range.end(); })
        void Append(TRange&& Range)
        {
            AppendRange(Range.begin(), Range.end());
        }

        TVector& operator+=(const TVector& Other) { Append(Other); return *this; }
        TVector& operator+=(TVector&& Other) { Append(std::move(Other)); return *this; }
        TVector& operator+=(std::initializer_list<T> Init) { Append(Init); return *this; }

        SizeType Insert(const T& Item, SizeType Index)
        {
            if (IsOwnedAddress(&Item))
            {
                EmplaceAt(Index, Item);
                return Index;
            }
            OpenGap(Index, 1);
            Memory::ConstructAt(Elements + Index, Item);
            ++ArrayNum;
            return Index;
        }

        SizeType Insert(T&& Item, SizeType Index)
        {
            if (IsOwnedAddress(&Item))
            {
                EmplaceAt(Index, std::move(Item));
                return Index;
            }
            OpenGap(Index, 1);
            Memory::ConstructAt(Elements + Index, std::move(Item));
            ++ArrayNum;
            return Index;
        }

        // A run out of our own buffer is copied first, since opening the gap can move or free it.
        SizeType Insert(const T* Ptr, SizeType Count, SizeType Index)
        {
            LUMINA_CONTAINER_CHECK(Count >= 0);
            if (Count <= 0)
            {
                return Index;
            }
            if (IsOwnedRegion(Ptr))
            {
                TVector Staged(Ptr, Count);
                return Insert(Staged.Elements, Count, Index);
            }
            OpenGap(Index, Count);
            ElementOps::CopyConstructRange(Elements + Index, Ptr, Count);
            ArrayNum += Count;
            return Index;
        }

        SizeType Insert(const TVector& Items, SizeType Index) { return Insert(Items.Elements, Items.ArrayNum, Index); }
        SizeType Insert(std::initializer_list<T> Init, SizeType Index) { return Insert(Init.begin(), static_cast<SizeType>(Init.size()), Index); }

        void InsertUninitialized(SizeType Index, SizeType Count = 1)
        {
            OpenGap(Index, Count);
            ArrayNum += Count;
        }

        void InsertDefaulted(SizeType Index, SizeType Count = 1)
        {
            OpenGap(Index, Count);
            ElementOps::DefaultConstructRange(Elements + Index, Count);
            ArrayNum += Count;
        }

        // The value is built before the gap opens, so an argument naming one of our elements stays valid.
        template <typename... TArgs>
        T& EmplaceAt(SizeType Index, TArgs&&... Args)
        {
            T Local(std::forward<TArgs>(Args)...);
            OpenGap(Index, 1);
            Memory::ConstructAt(Elements + Index, std::move(Local));
            ++ArrayNum;
            return Elements[Index];
        }

        void RemoveAt(SizeType Index, SizeType Count = 1)
        {
            LUMINA_CONTAINER_CHECK(Count >= 0 && Index >= 0 && Index <= ArrayNum && Count <= ArrayNum - Index);
            if (Count <= 0)
            {
                return;
            }
            ElementOps::DestructRange(Elements + Index, Count);
            ElementOps::RelocateRangeOverlapping(Elements + Index, Elements + Index + Count, ArrayNum - Index - Count);
            ArrayNum -= Count;
        }

        /** Removes in constant time by moving the last elements into the hole; does not preserve order. */
        void RemoveAtSwap(SizeType Index, SizeType Count = 1)
        {
            LUMINA_CONTAINER_CHECK(Count >= 0 && Index >= 0 && Index <= ArrayNum && Count <= ArrayNum - Index);
            if (Count <= 0)
            {
                return;
            }
            ElementOps::DestructRange(Elements + Index, Count);
            const SizeType Tail = ArrayNum - Index - Count;
            const SizeType Moved = Tail < Count ? Tail : Count;
            ElementOps::RelocateRange(Elements + Index, Elements + ArrayNum - Moved, Moved);
            ArrayNum -= Count;
        }

        SizeType Remove(const T& Item)
        {
            if (IsOwnedAddress(&Item))
            {
                const T Local(Item);
                return RemoveAll([&Local](const T& Element) { return Element == Local; });
            }
            return RemoveAll([&Item](const T& Element) { return Element == Item; });
        }

        SizeType RemoveSwap(const T& Item)
        {
            if (IsOwnedAddress(&Item))
            {
                const T Local(Item);
                return RemoveAllSwap([&Local](const T& Element) { return Element == Local; });
            }
            return RemoveAllSwap([&Item](const T& Element) { return Element == Item; });
        }

        SizeType RemoveSingle(const T& Item)
        {
            const SizeType Index = Find(Item);
            if (Index == Constants::kIndexNone)
            {
                return 0;
            }
            RemoveAt(Index);
            return 1;
        }

        SizeType RemoveSingleSwap(const T& Item)
        {
            const SizeType Index = Find(Item);
            if (Index == Constants::kIndexNone)
            {
                return 0;
            }
            RemoveAtSwap(Index);
            return 1;
        }

        /** Stable compaction in one pass; returns how many elements went away. */
        template <typename TPredicate>
        SizeType RemoveAll(TPredicate&& Predicate)
        {
            SizeType Write = 0;
            for (SizeType Read = 0; Read < ArrayNum; ++Read)
            {
                if (Predicate(Elements[Read]))
                {
                    ElementOps::DestructRange(Elements + Read, 1);
                    continue;
                }
                if (Write != Read)
                {
                    ElementOps::RelocateRange(Elements + Write, Elements + Read, 1);
                }
                ++Write;
            }
            const SizeType Removed = ArrayNum - Write;
            ArrayNum = Write;
            return Removed;
        }

        template <typename TPredicate>
        SizeType RemoveAllSwap(TPredicate&& Predicate)
        {
            SizeType Removed = 0;
            for (SizeType Index = 0; Index < ArrayNum;)
            {
                if (Predicate(Elements[Index]))
                {
                    RemoveAtSwap(Index);
                    ++Removed;
                }
                else
                {
                    ++Index;
                }
            }
            return Removed;
        }

        T Pop()
        {
            LUMINA_CONTAINER_CHECK(ArrayNum != 0);
            T Result = std::move(Elements[ArrayNum - 1]);
            --ArrayNum;
            ElementOps::DestructRange(Elements + ArrayNum, 1);
            return Result;
        }

        template <typename TKey>
        NODISCARD SizeType Find(const TKey& Item) const
        {
            return ElementOps::FindIndex(Elements, ArrayNum, Item);
        }

        template <typename TKey>
        NODISCARD bool Find(const TKey& Item, SizeType& OutIndex) const
        {
            OutIndex = Find(Item);
            return OutIndex != Constants::kIndexNone;
        }

        template <typename TKey>
        NODISCARD SizeType FindLast(const TKey& Item) const
        {
            for (SizeType Index = ArrayNum - 1; Index >= 0; --Index)
            {
                if (Elements[Index] == Item)
                {
                    return Index;
                }
            }
            return Constants::kIndexNone;
        }

        template <typename TPredicate>
        NODISCARD SizeType IndexOfByPredicate(TPredicate&& Predicate) const
        {
            for (SizeType Index = 0; Index < ArrayNum; ++Index)
            {
                if (Predicate(Elements[Index]))
                {
                    return Index;
                }
            }
            return Constants::kIndexNone;
        }

        template <typename TPredicate>
        NODISCARD SizeType FindLastByPredicate(TPredicate&& Predicate) const
        {
            for (SizeType Index = ArrayNum - 1; Index >= 0; --Index)
            {
                if (Predicate(Elements[Index]))
                {
                    return Index;
                }
            }
            return Constants::kIndexNone;
        }

        template <typename TKey>
        NODISCARD FORCEINLINE SizeType IndexOfByKey(const TKey& Key) const { return Find(Key); }

        template <typename TKey, typename TSelf>
        NODISCARD auto FindByKey(this TSelf&& Self, const TKey& Key) -> TLikeSelf<TSelf, T>*
        {
            const SizeType Index = Self.Find(Key);
            return Index != Constants::kIndexNone ? Self.Elements + Index : nullptr;
        }

        template <typename TPredicate, typename TSelf>
        NODISCARD auto FindByPredicate(this TSelf&& Self, TPredicate&& Predicate) -> TLikeSelf<TSelf, T>*
        {
            const SizeType Index = Self.IndexOfByPredicate(std::forward<TPredicate>(Predicate));
            return Index != Constants::kIndexNone ? Self.Elements + Index : nullptr;
        }

        template <typename TKey>
        NODISCARD FORCEINLINE bool Contains(const TKey& Item) const { return Find(Item) != Constants::kIndexNone; }

        template <typename TPredicate>
        NODISCARD FORCEINLINE bool ContainsByPredicate(TPredicate&& Predicate) const { return IndexOfByPredicate(std::forward<TPredicate>(Predicate)) != Constants::kIndexNone; }

        void Reserve(SizeType Number)
        {
            ReserveExact(static_cast<USizeType>(Number));
        }

        void Shrink()
        {
            if (ArrayNum == ArrayMax || IsInline())
            {
                return;
            }

            if constexpr (InlineCapacity > 0)
            {
                if (static_cast<size_t>(ArrayNum) <= InlineCapacity)
                {
                    T* Inline = FStorage::GetInlineData();
                    ElementOps::RelocateRange(Inline, Elements, ArrayNum);
                    ReleaseHeapBlock();
                    Elements = Inline;
                    ArrayMax = InlineCapacityV;
                    return;
                }
            }

            if (ArrayNum == 0)
            {
                ReleaseHeapBlock();
                ResetToEmpty();
                return;
            }

            ReallocateTo(static_cast<USizeType>(ArrayNum));
        }

        /** Destroys every element and sizes the block for Slack, so the default gives the memory back. */
        void Empty(SizeType Slack = 0)
        {
            ElementOps::DestructRange(Elements, ArrayNum);
            ArrayNum = 0;
            if (Slack <= InlineCapacityV)
            {
                ReleaseHeapBlock();
                ResetToEmpty();
            }
            else if (Slack != ArrayMax)
            {
                ReallocateTo(static_cast<USizeType>(CheckedCount(static_cast<USizeType>(Slack))));
            }
        }

        /** Destroys every element but keeps the block, so refilling it does not reallocate. */
        void Reset(SizeType NewSize = 0)
        {
            ElementOps::DestructRange(Elements, ArrayNum);
            ArrayNum = 0;
            if (static_cast<USizeType>(NewSize) > static_cast<USizeType>(ArrayMax))
            {
                ReserveExact(static_cast<USizeType>(NewSize));
            }
        }

        void SetNum(SizeType NewNum)
        {
            if (static_cast<USizeType>(NewNum) > static_cast<USizeType>(ArrayNum))
            {
                const SizeType Index = AddUninitialized(GrowthTo(NewNum));
                ElementOps::DefaultConstructRange(Elements + Index, ArrayNum - Index);
            }
            else if (NewNum < ArrayNum)
            {
                ElementOps::DestructRange(Elements + NewNum, ArrayNum - NewNum);
                ArrayNum = NewNum;
            }
        }

        void SetNum(SizeType NewNum, const T& Fill)
        {
            if (static_cast<USizeType>(NewNum) > static_cast<USizeType>(ArrayNum))
            {
                if (IsOwnedAddress(&Fill))
                {
                    const T Local(Fill);
                    SetNum(NewNum, Local);
                    return;
                }
                const SizeType Index = AddUninitialized(GrowthTo(NewNum));
                ElementOps::FillConstructRange(Elements + Index, ArrayNum - Index, Fill);
            }
            else if (NewNum < ArrayNum)
            {
                ElementOps::DestructRange(Elements + NewNum, ArrayNum - NewNum);
                ArrayNum = NewNum;
            }
        }

        void SetNumZeroed(SizeType NewNum)
        {
            static_assert(std::is_trivially_default_constructible_v<T>, "SetNumZeroed needs a T whose zero bytes are a valid value");
            if (static_cast<USizeType>(NewNum) > static_cast<USizeType>(ArrayNum))
            {
                AddZeroed(GrowthTo(NewNum));
            }
            else
            {
                ArrayNum = NewNum;
            }
        }

        // For a trivially-constructible T the caller writes every new element itself, so nothing is zeroed.
        void SetNumUninitialized(SizeType NewNum)
        {
            static_assert(std::is_trivially_default_constructible_v<T>, "SetNumUninitialized needs a trivial T");
            if (static_cast<USizeType>(NewNum) > static_cast<USizeType>(ArrayNum))
            {
                AddUninitialized(GrowthTo(NewNum));
            }
            else
            {
                ArrayNum = NewNum;
            }
        }

        void Init(const T& Element, SizeType Number)
        {
            if (IsOwnedAddress(&Element))
            {
                const T Local(Element);
                Init(Local, Number);
                return;
            }
            Reset(Number);
            SetNum(Number, Element);
        }

        void Swap(SizeType FirstIndex, SizeType SecondIndex)
        {
            LUMINA_CONTAINER_CHECK(IsValidIndex(FirstIndex) && IsValidIndex(SecondIndex));
            if (FirstIndex != SecondIndex)
            {
                ElementOps::SwapElements(Elements[FirstIndex], Elements[SecondIndex]);
            }
        }

        friend void swap(TVector& A, TVector& B) noexcept(Private::TNothrowRelocate<InlineCapacity, T>::value)
        {
            if (&A == &B)
            {
                return;
            }
            if constexpr (InlineCapacity == 0)
            {
                std::swap(A.Elements, B.Elements);
                std::swap(A.ArrayNum, B.ArrayNum);
                std::swap(A.ArrayMax, B.ArrayMax);
            }
            else
            {
                TVector Staged(std::move(A));
                A = std::move(B);
                B = std::move(Staged);
            }
        }

        NODISCARD friend bool operator==(const TVector& A, const TVector& B)
        {
            return A.ArrayNum == B.ArrayNum && ElementOps::RangeEquals(A.Elements, B.Elements, A.ArrayNum);
        }

        NODISCARD friend auto operator<=>(const TVector& A, const TVector& B)
        {
            return std::lexicographical_compare_three_way(A.begin(), A.end(), B.begin(), B.end());
        }

    private:

        template <typename, size_t, ContainerAllocatorType>
        friend class TVector;

        template <typename TIter>
        void AppendRange(TIter First, TIter Last)
        {
            if constexpr (std::contiguous_iterator<TIter> && std::is_same_v<std::remove_cv_t<std::iter_value_t<TIter>>, T>)
            {
                Append(std::to_address(First), CheckedCount(static_cast<uint64>(Last - First)));
            }
            else if constexpr (std::forward_iterator<TIter>)
            {
                // A range over our own elements, such as reverse iterators, is staged before growth can move it.
                if constexpr (std::is_lvalue_reference_v<std::iter_reference_t<TIter>>)
                {
                    if (First != Last && IsOwnedRegion(std::addressof(*First)))
                    {
                        TVector Staged(First, Last);
                        Append(std::move(Staged));
                        return;
                    }
                }
                const USizeType Count = static_cast<USizeType>(CheckedCount(static_cast<uint64>(std::distance(First, Last))));
                ReserveForGrowth(RequiredFor(static_cast<SizeType>(Count)));
                for (; First != Last; ++First)
                {
                    Memory::ConstructAt(Elements + ArrayNum, *First);
                    ++ArrayNum;
                }
            }
            else
            {
                for (; First != Last; ++First)
                {
                    Emplace(*First);
                }
            }
        }

        void TakeFrom(TVector& Other) noexcept(Private::TNothrowRelocate<InlineCapacity, T>::value)
        {
            if (Other.OwnsHeapBlock())
            {
                Elements = Other.Elements;
                ArrayNum = Other.ArrayNum;
                ArrayMax = Other.ArrayMax;
                Other.ResetToEmpty();
            }
            else if constexpr (InlineCapacity > 0)
            {
                ReserveExact(static_cast<USizeType>(Other.ArrayNum));
                ElementOps::RelocateRange(Elements, Other.Elements, Other.ArrayNum);
                ArrayNum = Other.ArrayNum;
                Other.ArrayNum = 0;
            }
        }

        FORCEINLINE constexpr void AdoptInlineStorage() noexcept
        {
            if constexpr (InlineCapacity > 0)
            {
                Elements = FStorage::GetInlineData();
                ArrayMax = InlineCapacityV;
            }
        }

        FORCEINLINE void ResetToEmpty() noexcept
        {
            Elements = FStorage::GetInlineData();
            ArrayNum = 0;
            ArrayMax = InlineCapacityV;
        }

        NODISCARD FORCEINLINE bool OwnsHeapBlock() const noexcept
        {
            return Elements != nullptr && !IsInline();
        }

        FORCEINLINE void ReleaseHeapBlock() noexcept
        {
            if (OwnsHeapBlock())
            {
                TAllocator::Deallocate(Elements, static_cast<size_t>(ArrayMax) * sizeof(T), alignof(T));
            }
        }

        NODISCARD FORCEINLINE bool IsOwnedAddress(const T* Address) const noexcept
        {
            return Address >= Elements && Address < Elements + ArrayNum;
        }

        NODISCARD FORCEINLINE bool IsOwnedRegion(const void* Address) const noexcept
        {
            const uint8* Byte = static_cast<const uint8*>(Address);
            return Byte >= reinterpret_cast<const uint8*>(Elements) && Byte < reinterpret_cast<const uint8*>(Elements + ArrayNum);
        }

        // Wraps instead of overflowing for a negative NewNum, which RequiredFor then reads as too large.
        NODISCARD FORCEINLINE SizeType GrowthTo(SizeType NewNum) const noexcept
        {
            return static_cast<SizeType>(static_cast<USizeType>(NewNum) - static_cast<USizeType>(ArrayNum));
        }

        // Read unsigned so a negative Added fails as too large; an int64 count must also refuse a sum that would wrap.
        NODISCARD FORCEINLINE uint64 RequiredFor(SizeType Added) const noexcept
        {
            if constexpr (sizeof(SizeType) <= 4)
            {
                return static_cast<uint64>(ArrayNum) + static_cast<USizeType>(Added);
            }
            else
            {
                const uint64 Extra = static_cast<uint64>(Added);
                const uint64 Room  = static_cast<uint64>(MaxElements) - static_cast<uint64>(ArrayNum);
                return Extra <= Room ? static_cast<uint64>(ArrayNum) + Extra : TNumericLimits<uint64>::Max();
            }
        }

        // An int32 count of any element fits size_t, while an int64 one can overflow it.
        NODISCARD static size_t BytesFor(USizeType Count)
        {
            if constexpr (sizeof(SizeType) > 4)
            {
                (void)Private::CheckCapacity(Count, TNumericLimits<uint64>::Max() / sizeof(T));
            }
            return static_cast<size_t>(Count) * sizeof(T);
        }

        NODISCARD static SizeType CheckedCount(uint64 Requested)
        {
            return static_cast<SizeType>(Private::CheckCapacity(Requested, static_cast<uint64>(MaxElements)));
        }

        FORCEINLINE void ReserveExact(USizeType NewMax)
        {
            const USizeType Wanted = static_cast<USizeType>(CheckedCount(NewMax));
            if (Wanted > static_cast<USizeType>(ArrayMax))
            {
                ReallocateTo(Wanted);
            }
        }

        // Exact reservations honor the request, and the growth paths come through here so they stay amortized.
        FORCEINLINE void ReserveForGrowth(uint64 Required)
        {
            if (Required > static_cast<uint64>(ArrayMax))
            {
                ReallocateTo(CalculateGrowth(ArrayMax, Required));
            }
        }

        // Measured against 1.5x and a hybrid: doubling wins, because 1.5x copies half again as many bytes.
        NODISCARD static USizeType CalculateGrowth(SizeType CurrentMax, uint64 Required)
        {
            uint64 Grown = (CurrentMax < 4) ? 4 : static_cast<uint64>(CurrentMax) * 2;
            if (Grown > static_cast<uint64>(MaxElements))
            {
                Grown = static_cast<uint64>(MaxElements);
            }
            if (Grown < Required)
            {
                Grown = Required;
            }
            return static_cast<USizeType>(CheckedCount(Grown));
        }

        void ReallocateTo(USizeType NewMax)
        {
            LUMINA_CONTAINER_CHECK(NewMax >= static_cast<USizeType>(ArrayNum));

            const size_t NewBytes = BytesFor(NewMax);

            if (NewMax > static_cast<USizeType>(ArrayMax) && OwnsHeapBlock())
            {
                if (TAllocator::TryExpand(Elements, static_cast<size_t>(ArrayMax) * sizeof(T), NewBytes))
                {
                    ArrayMax = static_cast<SizeType>(NewMax);
                    return;
                }
            }

            T* NewData = static_cast<T*>(TAllocator::Allocate(NewBytes, alignof(T)));
            ElementOps::RelocateRange(NewData, Elements, static_cast<USizeType>(ArrayNum));
            ReleaseHeapBlock();

            Elements = NewData;
            ArrayMax = static_cast<SizeType>(NewMax);
        }

        // Constructs into the new block before relocating, so an argument aliasing our own buffer stays live.
        // Never inlined, so the allocation path stays out of every Emplace call site.
        template <typename... TArgs>
        FORCENOINLINE T& EmplaceGrowing(TArgs&&... Args)
        {
            const USizeType NewMax   = CalculateGrowth(ArrayMax, static_cast<uint64>(ArrayMax) + 1);
            const size_t NewBytes = BytesFor(NewMax);

            if (OwnsHeapBlock() && TAllocator::TryExpand(Elements, static_cast<size_t>(ArrayMax) * sizeof(T), NewBytes))
            {
                ArrayMax = static_cast<SizeType>(NewMax);
                T* Slot = Elements + ArrayNum;
                Memory::ConstructAt(Slot, std::forward<TArgs>(Args)...);
                ++ArrayNum;
                return *Slot;
            }

            T* NewData = static_cast<T*>(TAllocator::Allocate(NewBytes, alignof(T)));
            T* Slot    = NewData + ArrayNum;
            Memory::ConstructAt(Slot, std::forward<TArgs>(Args)...);

            ElementOps::RelocateRange(NewData, Elements, ArrayNum);
            ReleaseHeapBlock();

            Elements = NewData;
            ArrayMax = static_cast<SizeType>(NewMax);
            ++ArrayNum;
            return *Slot;
        }

        FORCENOINLINE T& EmplaceGrowingValue(T Value)
        {
            return EmplaceGrowing(Value);
        }

        // Leaves Count raw slots at Index for the caller to construct into; ArrayNum is not updated here.
        void OpenGap(SizeType Index, SizeType Count)
        {
            LUMINA_CONTAINER_CHECK(Index >= 0 && Index <= ArrayNum && Count >= 0);
            ReserveForGrowth(RequiredFor(Count));
            ElementOps::RelocateRangeOverlapping(Elements + Index + Count, Elements + Index, ArrayNum - Index);
        }

        T*    Elements = nullptr;
        SizeType ArrayNum = 0;
        SizeType ArrayMax = 0;
    };

    template <typename T, size_t N>
    using TInlineVector = TVector<T, N>;

    template <typename T>
    using TScratchVector = TVector<T, 0, FScratchAllocator>;

    template <typename T>
    using TVector64 = TVector<T, 0, FHeapAllocator64>;
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

    template <typename T>
    using TVector64 = Containers::TVector64<T>;

    template <typename T, size_t N>
    using TInlineVector = Containers::TInlineVector<T, N>;
}
