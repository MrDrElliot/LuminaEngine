#pragma once

#include <bit>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "HashPrimitives.h"
#include "Pair.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define LUMINA_HASHTABLE_SSE2 1
    #include <emmintrin.h>
#else
    #define LUMINA_HASHTABLE_SSE2 0
#endif

namespace Lumina::Containers
{
    /** Hashes anything with a GetTypeHash found by ADL, which is what makes lookup heterogeneous by default. */
    struct FDefaultHash
    {
        using is_transparent = void;

        // Never re-mixed: a heterogeneous lookup only finds a key when both spellings hash identically.
        template <typename T>
        NODISCARD FORCEINLINE uint64 operator()(const T& Key) const noexcept
        {
            static_assert(requires { GetTypeHash(Key); },
                "No GetTypeHash for this key type. Declare one beside the type so ADL finds it.");
            return GetTypeHash(Key);
        }
    };

    struct FDefaultEqual
    {
        using is_transparent = void;

        template <typename TLeft, typename TRight>
        NODISCARD FORCEINLINE bool operator()(const TLeft& Left, const TRight& Right) const noexcept
        {
            return Left == Right;
        }
    };

namespace HashTableInternal
{
    using FCtrl = int8;

    inline constexpr FCtrl CtrlEmpty    = -128;
    inline constexpr FCtrl CtrlDeleted  = -2;
    inline constexpr FCtrl CtrlSentinel = -1;

    NODISCARD FORCEINLINE constexpr bool IsFull(FCtrl Value) noexcept { return Value >= 0; }
    NODISCARD FORCEINLINE constexpr bool IsEmpty(FCtrl Value) noexcept { return Value == CtrlEmpty; }
    NODISCARD FORCEINLINE constexpr bool IsDeleted(FCtrl Value) noexcept { return Value == CtrlDeleted; }
    NODISCARD FORCEINLINE constexpr bool IsEmptyOrDeleted(FCtrl Value) noexcept { return Value < CtrlSentinel; }

    /** H1 picks the group, H2 is the 7 bits stored in the control byte and matched a group at a time. */
    NODISCARD FORCEINLINE constexpr size_t H1(uint64 Hash) noexcept { return static_cast<size_t>(Hash >> 7); }
    NODISCARD FORCEINLINE constexpr FCtrl H2(uint64 Hash) noexcept { return static_cast<FCtrl>(Hash & 0x7F); }

    template <typename TWord, uint32 SignificantBits, uint32 Shift = 0>
    class TBitMask
    {
        static constexpr uint32 kTotalSignificantBits = SignificantBits << Shift;
        static constexpr uint32 kExtraBits = sizeof(TWord) * 8 - kTotalSignificantBits;

    public:

        explicit constexpr TBitMask(TWord InMask) noexcept : Mask(InMask) {}

        NODISCARD explicit constexpr operator bool() const noexcept { return Mask != 0; }
        NODISCARD constexpr uint32 operator*() const noexcept { return TrailingZeros(); }

        constexpr TBitMask& operator++() noexcept
        {
            Mask &= static_cast<TWord>(Mask - 1);
            return *this;
        }

        NODISCARD constexpr uint32 TrailingZeros() const noexcept
        {
            return static_cast<uint32>(std::countr_zero(Mask)) >> Shift;
        }

        NODISCARD constexpr uint32 LeadingZeros() const noexcept
        {
            return static_cast<uint32>(std::countl_zero(static_cast<TWord>(Mask << kExtraBits))) >> Shift;
        }

        NODISCARD constexpr TBitMask begin() const noexcept { return *this; }
        NODISCARD constexpr TBitMask end() const noexcept { return TBitMask(0); }

        NODISCARD friend constexpr bool operator==(TBitMask Left, TBitMask Right) noexcept { return Left.Mask == Right.Mask; }

    private:

        TWord Mask;
    };

#if LUMINA_HASHTABLE_SSE2
    struct FGroupSse2
    {
        static constexpr size_t kWidth = 16;
        using FMask = TBitMask<uint16, 16>;

        explicit FGroupSse2(const FCtrl* Position) noexcept
            : Ctrl(_mm_loadu_si128(reinterpret_cast<const __m128i*>(Position)))
        {}

        NODISCARD FMask Match(FCtrl Hash2) const noexcept
        {
            const __m128i Wanted = _mm_set1_epi8(static_cast<char>(Hash2));
            return FMask(static_cast<uint16>(_mm_movemask_epi8(_mm_cmpeq_epi8(Wanted, Ctrl))));
        }

        NODISCARD FMask MatchEmpty() const noexcept
        {
            const __m128i Wanted = _mm_set1_epi8(static_cast<char>(CtrlEmpty));
            return FMask(static_cast<uint16>(_mm_movemask_epi8(_mm_cmpeq_epi8(Wanted, Ctrl))));
        }

        /** Empty and deleted are the only bytes below the sentinel, so one signed compare separates them from full. */
        NODISCARD FMask MatchEmptyOrDeleted() const noexcept
        {
            const __m128i Special = _mm_set1_epi8(static_cast<char>(CtrlSentinel));
            return FMask(static_cast<uint16>(_mm_movemask_epi8(_mm_cmpgt_epi8(Special, Ctrl))));
        }

        NODISCARD FMask MatchFull() const noexcept
        {
            return FMask(static_cast<uint16>(static_cast<uint16>(_mm_movemask_epi8(Ctrl)) ^ uint16(0xFFFF)));
        }

        NODISCARD uint32 CountLeadingEmptyOrDeleted() const noexcept
        {
            const __m128i Special = _mm_set1_epi8(static_cast<char>(CtrlSentinel));
            const uint32 Bits = static_cast<uint32>(_mm_movemask_epi8(_mm_cmpgt_epi8(Special, Ctrl))) + 1;
            return static_cast<uint32>(std::countr_zero(Bits));
        }

        void ConvertSpecialToEmptyAndFullToDeleted(FCtrl* Destination) const noexcept
        {
            const __m128i MsbMask = _mm_set1_epi8(static_cast<char>(-128));
            const __m128i X126 = _mm_set1_epi8(126);
            const __m128i SpecialMask = _mm_cmpgt_epi8(_mm_setzero_si128(), Ctrl);
            const __m128i Result = _mm_or_si128(MsbMask, _mm_andnot_si128(SpecialMask, X126));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(Destination), Result);
        }

        __m128i Ctrl;
    };
#endif

    /** Byte-parallel fallback for any target without SSE2; half the group width, identical semantics. */
    struct FGroupPortable
    {
        static constexpr size_t kWidth = 8;
        static constexpr uint64 kLsbs = 0x0101010101010101ull;
        static constexpr uint64 kMsbs = 0x8080808080808080ull;

        using FMask = TBitMask<uint64, 8, 3>;

        explicit FGroupPortable(const FCtrl* Position) noexcept
        {
            std::memcpy(&Ctrl, Position, sizeof(Ctrl));
        }

        NODISCARD FMask Match(FCtrl Hash2) const noexcept
        {
            const uint64 Folded = Ctrl ^ (kLsbs * static_cast<uint8>(Hash2));
            return FMask((Folded - kLsbs) & ~Folded & kMsbs);
        }

        NODISCARD FMask MatchEmpty() const noexcept
        {
            return FMask((Ctrl & ~(Ctrl << 6)) & kMsbs);
        }

        NODISCARD FMask MatchEmptyOrDeleted() const noexcept
        {
            return FMask((Ctrl & ~(Ctrl << 7)) & kMsbs);
        }

        NODISCARD FMask MatchFull() const noexcept
        {
            return FMask((Ctrl ^ kMsbs) & kMsbs);
        }

        NODISCARD uint32 CountLeadingEmptyOrDeleted() const noexcept
        {
            return static_cast<uint32>((std::countr_zero((Ctrl | ~(Ctrl >> 7)) & kLsbs) + 7) >> 3);
        }

        void ConvertSpecialToEmptyAndFullToDeleted(FCtrl* Destination) const noexcept
        {
            const uint64 Special = Ctrl & kMsbs;
            const uint64 Result = (~Special + (Special >> 7)) & ~kLsbs;
            std::memcpy(Destination, &Result, sizeof(Result));
        }

        uint64 Ctrl;
    };

#if LUMINA_HASHTABLE_SSE2
    using FGroup = FGroupSse2;
#else
    using FGroup = FGroupPortable;
#endif

    inline constexpr size_t kGroupWidth = FGroup::kWidth;

    NODISCARD FORCEINLINE constexpr bool IsValidCapacity(size_t Capacity) noexcept
    {
        return Capacity > 0 && ((Capacity + 1) & Capacity) == 0;
    }

    NODISCARD FORCEINLINE constexpr size_t NormalizeCapacity(size_t Count) noexcept
    {
        return Count == 0 ? 1 : (~size_t(0) >> std::countl_zero(Count));
    }

    /** The table grows at 7/8, which is the load the group probe absorbs without long chains. */
    NODISCARD FORCEINLINE constexpr size_t CapacityToGrowth(size_t Capacity) noexcept
    {
        return Capacity - Capacity / 8;
    }

    NODISCARD FORCEINLINE constexpr size_t GrowthToLowerBoundCapacity(size_t Growth) noexcept
    {
        return Growth + static_cast<size_t>((static_cast<int64>(Growth) - 1) / 7);
    }

    NODISCARD FORCEINLINE constexpr size_t NumCtrlBytes(size_t Capacity) noexcept
    {
        return Capacity + 1 + kGroupWidth;
    }

    NODISCARD FORCEINLINE constexpr size_t SlotOffset(size_t Capacity, size_t SlotAlign) noexcept
    {
        return (NumCtrlBytes(Capacity) + SlotAlign - 1) & ~(SlotAlign - 1);
    }

    NODISCARD FORCEINLINE constexpr size_t AllocSize(size_t Capacity, size_t SlotSize, size_t SlotAlign) noexcept
    {
        return SlotOffset(Capacity, SlotAlign) + Capacity * SlotSize;
    }

    /** A capacity-0 table probes this instead of branching on null; the leading sentinel stops every search. */
    NODISCARD inline FCtrl* EmptyGroup() noexcept
    {
        alignas(16) static constexpr FCtrl Group[] =
        {
            CtrlSentinel, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty,
            CtrlEmpty,    CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty,
        };
        return const_cast<FCtrl*>(Group);
    }

    inline void ResetCtrl(FCtrl* Ctrl, size_t Capacity) noexcept
    {
        std::memset(Ctrl, CtrlEmpty, NumCtrlBytes(Capacity));
        Ctrl[Capacity] = CtrlSentinel;
    }

    /** The mirrored write keeps the cloned tail bytes in step, so a group load near the end stays valid. */
    FORCEINLINE void SetCtrl(FCtrl* Ctrl, size_t Capacity, size_t Index, FCtrl Value) noexcept
    {
        Ctrl[Index] = Value;
        Ctrl[((Index - kGroupWidth) & Capacity) + 1 + ((kGroupWidth - 1) & Capacity)] = Value;
    }

    inline void ConvertDeletedToEmptyAndFullToDeleted(FCtrl* Ctrl, size_t Capacity) noexcept
    {
        for (FCtrl* Position = Ctrl; Position != Ctrl + Capacity + 1; Position += kGroupWidth)
        {
            FGroup(Position).ConvertSpecialToEmptyAndFullToDeleted(Position);
        }
        std::memcpy(Ctrl + Capacity + 1, Ctrl, kGroupWidth);
        Ctrl[Capacity] = CtrlSentinel;
    }

    class FProbeSeq
    {
    public:

        FProbeSeq(size_t Hash, size_t InMask) noexcept
            : Mask(InMask)
            , Offset(Hash & InMask)
        {}

        NODISCARD FORCEINLINE size_t GetOffset() const noexcept { return Offset; }
        NODISCARD FORCEINLINE size_t GetOffset(size_t Slot) const noexcept { return (Offset + Slot) & Mask; }
        NODISCARD FORCEINLINE size_t GetIndex() const noexcept { return Index; }

        /** Triangular steps visit every group exactly once when the capacity is a power of two minus one. */
        FORCEINLINE void Next() noexcept
        {
            Index += kGroupWidth;
            Offset += Index;
            Offset &= Mask;
        }

    private:

        size_t Mask;
        size_t Offset;
        size_t Index = 0;
    };

    struct FFindInfo
    {
        size_t Offset;
        size_t ProbeLength;
    };

    inline FFindInfo FindFirstNonFull(const FCtrl* Ctrl, uint64 Hash, size_t Capacity) noexcept
    {
        FProbeSeq Seq(H1(Hash), Capacity);
        while (true)
        {
            const FGroup Group(Ctrl + Seq.GetOffset());
            if (const auto Mask = Group.MatchEmptyOrDeleted())
            {
                return { Seq.GetOffset(*Mask), Seq.GetIndex() };
            }
            Seq.Next();
            LUMINA_CONTAINER_CHECK_WITHIN(Seq.GetIndex(), Capacity);
        }
    }

    template <typename T>
    FORCEINLINE void RelocateOne(T* Destination, T* Source) noexcept
    {
        if constexpr (TIsTriviallyRelocatable_V<T>)
        {
            std::memcpy(static_cast<void*>(Destination), static_cast<const void*>(Source), sizeof(T));
        }
        else
        {
            new (static_cast<void*>(Destination)) T(std::move(*Source));
            Source->~T();
        }
    }
}

namespace Private
{
    template <size_t Bytes, size_t Align>
    struct THashInlineStorage
    {
        NODISCARD FORCEINLINE void* GetInlineBlock() noexcept { return Buffer; }
        NODISCARD FORCEINLINE const void* GetInlineBlock() const noexcept { return Buffer; }

        alignas(Align) uint8 Buffer[Bytes];
    };

    /** Empty base, so a table with no inline capacity stays at five words. */
    template <size_t Align>
    struct THashInlineStorage<0, Align>
    {
        NODISCARD FORCEINLINE void* GetInlineBlock() noexcept { return nullptr; }
        NODISCARD FORCEINLINE const void* GetInlineBlock() const noexcept { return nullptr; }
    };

    template <typename TPolicy, size_t InlineCapacity>
    using THashStorageFor = THashInlineStorage<
        (InlineCapacity == 0
            ? 0
            : HashTableInternal::AllocSize(HashTableInternal::NormalizeCapacity(InlineCapacity),
                                           sizeof(typename TPolicy::slot_type),
                                           alignof(typename TPolicy::slot_type))),
        (alignof(typename TPolicy::slot_type) > 16 ? alignof(typename TPolicy::slot_type) : 16)>;
}

    template <typename T>
    struct TSetPolicy
    {
        using slot_type   = T;
        using key_type    = T;
        using value_type  = T;
        using init_type   = T;
        using mapped_type = void;

        static constexpr bool bTrivialDestroy = std::is_trivially_destructible_v<T>;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return Slot; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return Slot; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return Slot; }

        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            new (static_cast<void*>(Slot)) T(std::forward<TArgs>(Args)...);
        }

        FORCEINLINE static void Destroy(slot_type* Slot) noexcept { Slot->~T(); }

        FORCEINLINE static void Transfer(slot_type* Destination, slot_type* Source) noexcept
        {
            HashTableInternal::RelocateOne(Destination, Source);
        }
    };

    template <typename K, typename V>
    struct TMapPolicy
    {
        using FMutablePair = TPair<K, V>;

        /** The caller sees a const key, which cannot be moved; the mutable twin is what a rehash relocates. */
        union FMapSlot
        {
            TPair<const K, V> Value;
            FMutablePair      Mutable;

            FMapSlot() noexcept {}
            ~FMapSlot() noexcept {}
        };

        using slot_type   = FMapSlot;
        using key_type    = K;
        using value_type  = TPair<const K, V>;
        using init_type   = TPair<K, V>;
        using mapped_type = V;

        static constexpr bool bTrivialDestroy = std::is_trivially_destructible_v<FMutablePair>;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return Slot.Value.first; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return Slot.Value; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return Slot.Value; }
        NODISCARD FORCEINLINE static V& Value(slot_type& Slot) noexcept { return Slot.Mutable.second; }

        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            new (static_cast<void*>(&Slot->Mutable)) FMutablePair(std::forward<TArgs>(Args)...);
        }

        FORCEINLINE static void Destroy(slot_type* Slot) noexcept { Slot->Mutable.~FMutablePair(); }

        FORCEINLINE static void Transfer(slot_type* Destination, slot_type* Source) noexcept
        {
            HashTableInternal::RelocateOne(&Destination->Mutable, &Source->Mutable);
        }
    };

    /** Slot is a pointer to a separately allocated element, so an element address survives every rehash. */
    template <typename T, ContainerAllocatorType TAllocator>
    struct TNodeSetPolicy
    {
        using slot_type   = T*;
        using key_type    = T;
        using value_type  = T;
        using init_type   = T;
        using mapped_type = void;

        static constexpr bool bTrivialDestroy = false;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return *Slot; }

        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            void* Block = TAllocator::Allocate(sizeof(T), alignof(T));
            *Slot = new (Block) T(std::forward<TArgs>(Args)...);
        }

        FORCEINLINE static void Destroy(slot_type* Slot) noexcept
        {
            (*Slot)->~T();
            TAllocator::Deallocate(*Slot, sizeof(T), alignof(T));
            *Slot = nullptr;
        }

        /** Only the pointer moves, which is exactly why the element address is stable. */
        FORCEINLINE static void Transfer(slot_type* Destination, slot_type* Source) noexcept
        {
            *Destination = *Source;
            *Source = nullptr;
        }
    };

    template <typename K, typename V, ContainerAllocatorType TAllocator>
    struct TNodeMapPolicy
    {
        using FNode = TPair<const K, V>;

        using slot_type   = FNode*;
        using key_type    = K;
        using value_type  = FNode;
        using init_type   = TPair<K, V>;
        using mapped_type = V;

        static constexpr bool bTrivialDestroy = false;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return Slot->first; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static V& Value(slot_type& Slot) noexcept { return Slot->second; }

        // The node never moves, so the key can stay const here and no mutable twin is needed.
        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            void* Block = TAllocator::Allocate(sizeof(FNode), alignof(FNode));
            *Slot = new (Block) FNode(std::forward<TArgs>(Args)...);
        }

        FORCEINLINE static void Destroy(slot_type* Slot) noexcept
        {
            (*Slot)->~FNode();
            TAllocator::Deallocate(*Slot, sizeof(FNode), alignof(FNode));
            *Slot = nullptr;
        }

        FORCEINLINE static void Transfer(slot_type* Destination, slot_type* Source) noexcept
        {
            *Destination = *Source;
            *Source = nullptr;
        }
    };

    /** Open-addressed table with one control byte per slot, probed a whole group at a time. */
    template <typename TPolicy, typename THasher, typename TEqual, ContainerAllocatorType TAllocator, size_t InlineCapacity = 0>
    class TRawHashTable : private Private::THashStorageFor<TPolicy, InlineCapacity>
    {
        using FStorage = Private::THashStorageFor<TPolicy, InlineCapacity>;

    protected:

        using FCtrl = HashTableInternal::FCtrl;
        using FGroup = HashTableInternal::FGroup;

        static constexpr size_t kNoIndex = ~static_cast<size_t>(0);
        static constexpr size_t kSlotAlign = alignof(typename TPolicy::slot_type);
        static constexpr size_t kBlockAlign = kSlotAlign > 16 ? kSlotAlign : 16;
        static constexpr size_t kInlineCapacity = InlineCapacity == 0 ? 0 : HashTableInternal::NormalizeCapacity(InlineCapacity);

    public:

        using policy_type     = TPolicy;
        using slot_type       = typename TPolicy::slot_type;
        using key_type        = typename TPolicy::key_type;
        using value_type      = typename TPolicy::value_type;
        using init_type       = typename TPolicy::init_type;
        using size_type       = size_t;
        using difference_type = ptrdiff_t;
        using hasher          = THasher;
        using key_equal       = TEqual;
        using allocator_type  = TAllocator;
        using reference       = value_type&;
        using const_reference = const value_type&;
        using pointer         = value_type*;
        using const_pointer   = const value_type*;
        using mapped_type     = typename TPolicy::mapped_type;

        static constexpr size_t InlineCapacityV = kInlineCapacity;

        template <bool bConst>
        class TIterator
        {
            friend class TRawHashTable;
            template <bool> friend class TIterator;

            using FTableSlot = std::conditional_t<bConst, const slot_type, slot_type>;

        public:

            using iterator_category = std::forward_iterator_tag;
            using value_type        = typename TPolicy::value_type;
            using difference_type   = ptrdiff_t;
            using reference         = std::conditional_t<bConst, const value_type&, value_type&>;
            using pointer           = std::conditional_t<bConst, const value_type*, value_type*>;

            TIterator() noexcept = default;

            // Templated so it is never the copy constructor, which would suppress the implicit one.
            template <bool bOther>
            requires (bConst && !bOther)
            TIterator(const TIterator<bOther>& Other) noexcept
                : Ctrl(Other.Ctrl)
                , Slot(Other.Slot)
            {}

            NODISCARD reference operator*() const noexcept
            {
                LUMINA_CONTAINER_CHECK(Ctrl != nullptr);
                return TPolicy::Element(*Slot);
            }

            NODISCARD pointer operator->() const noexcept { return &operator*(); }

            TIterator& operator++() noexcept
            {
                ++Ctrl;
                ++Slot;
                SkipEmptyOrDeleted();
                return *this;
            }

            TIterator operator++(int) noexcept
            {
                TIterator Copy = *this;
                ++*this;
                return Copy;
            }

            NODISCARD friend bool operator==(const TIterator& Left, const TIterator& Right) noexcept
            {
                return Left.Ctrl == Right.Ctrl;
            }

            // A const and a mutable iterator into the same table have to compare, which one signature cannot do.
            NODISCARD friend bool operator==(const TIterator& Left, const TIterator<!bConst>& Right) noexcept
            {
                return Left.Ctrl == Right.Ctrl;
            }

        private:

            TIterator(const FCtrl* InCtrl, FTableSlot* InSlot) noexcept
                : Ctrl(InCtrl)
                , Slot(InSlot)
            {}

            void SkipEmptyOrDeleted() noexcept
            {
                while (HashTableInternal::IsEmptyOrDeleted(*Ctrl))
                {
                    const uint32 Shift = FGroup(Ctrl).CountLeadingEmptyOrDeleted();
                    Ctrl += Shift;
                    Slot += Shift;
                }
                if (*Ctrl == HashTableInternal::CtrlSentinel)
                {
                    Ctrl = nullptr;
                    Slot = nullptr;
                }
            }

            const FCtrl* Ctrl = nullptr;
            FTableSlot*  Slot = nullptr;
        };

        using iterator       = TIterator<false>;
        using const_iterator = TIterator<true>;

        TRawHashTable() noexcept { ResetToInline(); }

        explicit TRawHashTable(size_t InitialCapacity)
        {
            ResetToInline();
            reserve(InitialCapacity);
        }

        TRawHashTable(const TRawHashTable& Other)
        {
            ResetToInline();
            reserve(Other.Size);
            for (const auto& Element : Other)
            {
                InsertValue(Element);
            }
        }

        TRawHashTable(TRawHashTable&& Other) noexcept
            : Hasher(std::move(Other.Hasher))
            , Equal(std::move(Other.Equal))
        {
            ResetToInline();
            AdoptFrom(Other);
        }

        TRawHashTable(std::initializer_list<init_type> Values)
        {
            ResetToInline();
            reserve(Values.size());
            for (const init_type& Value : Values)
            {
                InsertValue(Value);
            }
        }

        ~TRawHashTable() { ReleaseBlock(); }

        TRawHashTable& operator=(const TRawHashTable& Other)
        {
            if (this != &Other)
            {
                clear();
                reserve(Other.Size);
                for (const auto& Element : Other)
                {
                    InsertValue(Element);
                }
            }
            return *this;
        }

        TRawHashTable& operator=(TRawHashTable&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseBlock();
                Hasher = std::move(Other.Hasher);
                Equal  = std::move(Other.Equal);
                AdoptFrom(Other);
            }
            return *this;
        }

        NODISCARD FORCEINLINE size_t size() const noexcept { return Size; }
        NODISCARD FORCEINLINE size_t capacity() const noexcept { return Capacity; }
        NODISCARD FORCEINLINE bool empty() const noexcept { return Size == 0; }
        NODISCARD static constexpr size_t max_size() noexcept { return ~static_cast<size_t>(0) / 2; }

        NODISCARD FORCEINLINE float load_factor() const noexcept
        {
            return Capacity == 0 ? 0.0f : static_cast<float>(Size) / static_cast<float>(Capacity);
        }

        NODISCARD iterator begin() noexcept
        {
            iterator It(Ctrl, Slots);
            It.SkipEmptyOrDeleted();
            return It;
        }

        NODISCARD const_iterator begin() const noexcept
        {
            const_iterator It(Ctrl, Slots);
            It.SkipEmptyOrDeleted();
            return It;
        }

        NODISCARD FORCEINLINE iterator end() noexcept { return iterator(); }
        NODISCARD FORCEINLINE const_iterator end() const noexcept { return const_iterator(); }
        NODISCARD FORCEINLINE const_iterator cbegin() const noexcept { return begin(); }
        NODISCARD FORCEINLINE const_iterator cend() const noexcept { return const_iterator(); }

        void clear() noexcept
        {
            DestroyAllSlots();
            Size = 0;
            if (Capacity != 0)
            {
                HashTableInternal::ResetCtrl(Ctrl, Capacity);
                GrowthLeft = HashTableInternal::CapacityToGrowth(Capacity);
            }
        }

        void reserve(size_t Count)
        {
            if (Count > Size + GrowthLeft)
            {
                Resize(HashTableInternal::NormalizeCapacity(HashTableInternal::GrowthToLowerBoundCapacity(Count)));
            }
        }

        void rehash(size_t Count)
        {
            if (Count == 0 && Size == 0)
            {
                ReleaseBlock();
                return;
            }

            const size_t LowerBound = HashTableInternal::GrowthToLowerBoundCapacity(Size);
            const size_t NewCapacity = HashTableInternal::NormalizeCapacity(Count < LowerBound ? LowerBound : Count);
            if (NewCapacity != Capacity)
            {
                Resize(NewCapacity);
            }
        }

        void shrink_to_fit() { rehash(0); }

        template <typename TKeyArg>
        NODISCARD iterator find(const TKeyArg& Key) noexcept
        {
            const size_t Index = FindIndex(Key, Hasher(Key));
            return Index == kNoIndex ? end() : iterator(Ctrl + Index, Slots + Index);
        }

        template <typename TKeyArg>
        NODISCARD const_iterator find(const TKeyArg& Key) const noexcept
        {
            const size_t Index = FindIndex(Key, Hasher(Key));
            return Index == kNoIndex ? end() : const_iterator(Ctrl + Index, Slots + Index);
        }

        /** Looks up with a caller-supplied hash and equality, for a key type the table is not built on. */
        template <typename TKeyArg, typename THashFn, typename TEqualFn>
        NODISCARD iterator find_as(const TKeyArg& Key, THashFn&& HashFn, TEqualFn&& EqualFn) noexcept
        {
            const size_t Index = FindIndexWith(Key, static_cast<uint64>(HashFn(Key)), EqualFn);
            return Index == kNoIndex ? end() : IteratorAt(Index);
        }

        template <typename TKeyArg, typename THashFn, typename TEqualFn>
        NODISCARD const_iterator find_as(const TKeyArg& Key, THashFn&& HashFn, TEqualFn&& EqualFn) const noexcept
        {
            const size_t Index = FindIndexWith(Key, static_cast<uint64>(HashFn(Key)), EqualFn);
            return Index == kNoIndex ? end() : const_iterator(Ctrl + Index, Slots + Index);
        }

        template <typename TKeyArg>
        NODISCARD FORCEINLINE bool contains(const TKeyArg& Key) const noexcept
        {
            return FindIndex(Key, Hasher(Key)) != kNoIndex;
        }

        template <typename TKeyArg>
        NODISCARD FORCEINLINE size_t count(const TKeyArg& Key) const noexcept
        {
            return contains(Key) ? 1 : 0;
        }

        TPair<iterator, bool> insert(const init_type& Value) { return InsertValue(Value); }
        TPair<iterator, bool> insert(init_type&& Value) { return InsertValue(std::move(Value)); }

        iterator insert(const_iterator, const init_type& Value) { return InsertValue(Value).first; }
        iterator insert(const_iterator, init_type&& Value) { return InsertValue(std::move(Value)).first; }

        template <typename TIter>
        void insert(TIter First, TIter Last)
        {
            for (; First != Last; ++First)
            {
                InsertValue(*First);
            }
        }

        void insert(std::initializer_list<init_type> Values)
        {
            reserve(Size + Values.size());
            for (const init_type& Value : Values)
            {
                InsertValue(Value);
            }
        }

        /** Staging the element first is what lets an arbitrary argument pack find its own key. */
        template <typename... TArgs>
        TPair<iterator, bool> emplace(TArgs&&... Args)
        {
            alignas(slot_type) unsigned char Raw[sizeof(slot_type)];
            slot_type* Staged = reinterpret_cast<slot_type*>(Raw);
            TPolicy::Construct(Staged, std::forward<TArgs>(Args)...);

            const uint64 Hash = Hasher(TPolicy::Key(*Staged));
            const size_t Existing = FindIndex(TPolicy::Key(*Staged), Hash);
            if (Existing != kNoIndex)
            {
                TPolicy::Destroy(Staged);
                return { IteratorAt(Existing), false };
            }

            const size_t Index = PrepareInsert(Hash);
            TPolicy::Transfer(Slots + Index, Staged);
            return { IteratorAt(Index), true };
        }

        template <typename... TArgs>
        iterator emplace_hint(const_iterator, TArgs&&... Args)
        {
            return emplace(std::forward<TArgs>(Args)...).first;
        }

        // Constrained so an iterator argument cannot bind here instead of the iterator overload below.
        template <typename TKeyArg>
        requires (!std::is_convertible_v<const TKeyArg&, const_iterator>)
        size_t erase(const TKeyArg& Key) noexcept
        {
            const size_t Index = FindIndex(Key, Hasher(Key));
            if (Index == kNoIndex)
            {
                return 0;
            }
            EraseAt(Index);
            return 1;
        }

        iterator erase(const_iterator Position) noexcept
        {
            LUMINA_CONTAINER_CHECK(Position.Ctrl != nullptr);
            const size_t Index = static_cast<size_t>(Position.Ctrl - Ctrl);
            EraseAt(Index);

            iterator Next(Ctrl + Index, Slots + Index);
            Next.SkipEmptyOrDeleted();
            return Next;
        }

        iterator erase(const_iterator First, const_iterator Last) noexcept
        {
            while (First != Last)
            {
                First = const_iterator(erase(First));
            }
            return iterator(First.Ctrl, const_cast<slot_type*>(First.Slot));
        }

        void swap(TRawHashTable& Other) noexcept
        {
            if (this == &Other)
            {
                return;
            }

            if constexpr (kInlineCapacity == 0)
            {
                std::swap(Ctrl, Other.Ctrl);
                std::swap(Slots, Other.Slots);
                std::swap(Size, Other.Size);
                std::swap(Capacity, Other.Capacity);
                std::swap(GrowthLeft, Other.GrowthLeft);
                std::swap(Hasher, Other.Hasher);
                std::swap(Equal, Other.Equal);
            }
            else
            {
                TRawHashTable Staged(std::move(*this));
                *this = std::move(Other);
                Other = std::move(Staged);
            }
        }

        /** True while the table still lives in its inline block rather than a heap allocation. */
        NODISCARD FORCEINLINE bool IsInline() const noexcept
        {
            if constexpr (kInlineCapacity == 0)
            {
                return false;
            }
            else
            {
                return static_cast<const void*>(Ctrl) == FStorage::GetInlineBlock();
            }
        }

        NODISCARD hasher hash_function() const { return Hasher; }
        NODISCARD key_equal key_eq() const { return Equal; }

        /** Bytes held by the table, so a memory report can attribute them without guessing. */
        NODISCARD size_t GetAllocatedBytes() const noexcept
        {
            return (Capacity == 0 || IsInline()) ? 0 : HashTableInternal::AllocSize(Capacity, sizeof(slot_type), kSlotAlign);
        }

    protected:

        template <typename TKeyArg>
        NODISCARD size_t FindIndex(const TKeyArg& Key, uint64 Hash) const noexcept
        {
            HashTableInternal::FProbeSeq Seq(HashTableInternal::H1(Hash), Capacity);
            const FCtrl Wanted = HashTableInternal::H2(Hash);

            while (true)
            {
                const FGroup Group(Ctrl + Seq.GetOffset());
                for (uint32 Bit : Group.Match(Wanted))
                {
                    const size_t Index = Seq.GetOffset(Bit);
                    if (Equal(TPolicy::Key(Slots[Index]), Key)) [[likely]]
                    {
                        return Index;
                    }
                }
                if (Group.MatchEmpty()) [[likely]]
                {
                    return kNoIndex;
                }
                Seq.Next();
                LUMINA_CONTAINER_CHECK_WITHIN(Seq.GetIndex(), Capacity);
            }
        }

        template <typename TKeyArg, typename TEqualFn>
        NODISCARD size_t FindIndexWith(const TKeyArg& Key, uint64 Hash, TEqualFn& EqualFn) const noexcept
        {
            HashTableInternal::FProbeSeq Seq(HashTableInternal::H1(Hash), Capacity);
            const FCtrl Wanted = HashTableInternal::H2(Hash);

            while (true)
            {
                const FGroup Group(Ctrl + Seq.GetOffset());
                for (uint32 Bit : Group.Match(Wanted))
                {
                    const size_t Index = Seq.GetOffset(Bit);
                    if (EqualFn(TPolicy::Key(Slots[Index]), Key))
                    {
                        return Index;
                    }
                }
                if (Group.MatchEmpty())
                {
                    return kNoIndex;
                }
                Seq.Next();
                LUMINA_CONTAINER_CHECK_WITHIN(Seq.GetIndex(), Capacity);
            }
        }

        size_t PrepareInsert(uint64 Hash)
        {
            HashTableInternal::FFindInfo Target = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Capacity);
            if (GrowthLeft == 0 && !HashTableInternal::IsDeleted(Ctrl[Target.Offset]))
            {
                RehashAndGrowIfNecessary();
                Target = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Capacity);
            }

            ++Size;
            GrowthLeft -= HashTableInternal::IsEmpty(Ctrl[Target.Offset]) ? 1 : 0;
            HashTableInternal::SetCtrl(Ctrl, Capacity, Target.Offset, HashTableInternal::H2(Hash));
            return Target.Offset;
        }

        template <typename TValue>
        TPair<iterator, bool> InsertValue(TValue&& Value)
        {
            const uint64 Hash = Hasher(KeyOf(Value));
            const size_t Existing = FindIndex(KeyOf(Value), Hash);
            if (Existing != kNoIndex)
            {
                return { IteratorAt(Existing), false };
            }

            const size_t Index = PrepareInsert(Hash);
            TPolicy::Construct(Slots + Index, std::forward<TValue>(Value));
            return { IteratorAt(Index), true };
        }

        /** Finds Key, or claims a slot and reports that the caller still has to build the element in it. */
        template <typename TKeyArg>
        TPair<size_t, bool> FindOrPrepareInsert(const TKeyArg& Key)
        {
            const uint64 Hash = Hasher(Key);
            const size_t Existing = FindIndex(Key, Hash);
            if (Existing != kNoIndex)
            {
                return { Existing, false };
            }
            return { PrepareInsert(Hash), true };
        }

        void EraseAt(size_t Index) noexcept
        {
            TPolicy::Destroy(Slots + Index);
            EraseMetaOnly(Index);
        }

        NODISCARD FORCEINLINE slot_type* SlotAt(size_t Index) noexcept { return Slots + Index; }
        NODISCARD FORCEINLINE iterator IteratorAt(size_t Index) noexcept { return iterator(Ctrl + Index, Slots + Index); }

    private:

        template <typename TValue>
        NODISCARD FORCEINLINE static decltype(auto) KeyOf(const TValue& Value) noexcept
        {
            if constexpr (std::is_same_v<init_type, key_type>)
            {
                return (Value);
            }
            else
            {
                return (Value.first);
            }
        }

        void AdoptEmpty() noexcept
        {
            Ctrl       = HashTableInternal::EmptyGroup();
            Slots      = nullptr;
            Size       = 0;
            Capacity   = 0;
            GrowthLeft = 0;
        }

        void ResetToInline() noexcept
        {
            if constexpr (kInlineCapacity == 0)
            {
                AdoptEmpty();
            }
            else
            {
                PointAtBlock(FStorage::GetInlineBlock(), kInlineCapacity);
                Size = 0;
                GrowthLeft = HashTableInternal::CapacityToGrowth(kInlineCapacity);
            }
        }

        FORCEINLINE void PointAtBlock(void* Block, size_t NewCapacity) noexcept
        {
            Ctrl = static_cast<FCtrl*>(Block);
            Slots = reinterpret_cast<slot_type*>(static_cast<uint8*>(Block) + HashTableInternal::SlotOffset(NewCapacity, kSlotAlign));
            Capacity = NewCapacity;
            HashTableInternal::ResetCtrl(Ctrl, Capacity);
        }

        /** A heap table hands over its pointers; an inline one has to move its elements slot for slot. */
        void AdoptFrom(TRawHashTable& Other) noexcept
        {
            if (!Other.IsInline())
            {
                ReleaseBlock();
                Ctrl       = Other.Ctrl;
                Slots      = Other.Slots;
                Size       = Other.Size;
                Capacity   = Other.Capacity;
                GrowthLeft = Other.GrowthLeft;
                Other.ResetToInline();
                return;
            }

            if constexpr (kInlineCapacity != 0)
            {
                ResetToInline();

                // Both blocks have the same capacity, so every element keeps its slot and no rehash is needed.
                std::memcpy(Ctrl, Other.Ctrl, HashTableInternal::NumCtrlBytes(Capacity));
                for (size_t Index = 0; Index != Capacity; ++Index)
                {
                    if (HashTableInternal::IsFull(Ctrl[Index]))
                    {
                        TPolicy::Transfer(Slots + Index, Other.Slots + Index);
                    }
                }
                Size = Other.Size;
                GrowthLeft = Other.GrowthLeft;

                Other.Size = 0;
                HashTableInternal::ResetCtrl(Other.Ctrl, Other.Capacity);
                Other.GrowthLeft = HashTableInternal::CapacityToGrowth(Other.Capacity);
            }
        }

        void DestroyAllSlots() noexcept
        {
            if (Capacity == 0 || Size == 0)
            {
                return;
            }
            if constexpr (!TPolicy::bTrivialDestroy)
            {
                for (size_t Index = 0; Index != Capacity; ++Index)
                {
                    if (HashTableInternal::IsFull(Ctrl[Index]))
                    {
                        TPolicy::Destroy(Slots + Index);
                    }
                }
            }
        }

        void ReleaseBlock() noexcept
        {
            if (Capacity == 0)
            {
                return;
            }
            DestroyAllSlots();
            if (!IsInline())
            {
                TAllocator::Deallocate(Ctrl, HashTableInternal::AllocSize(Capacity, sizeof(slot_type), kSlotAlign), kBlockAlign);
            }
            ResetToInline();
        }

        /** An erase whose group still holds an empty byte can clear the slot outright: no probe ran past it. */
        void EraseMetaOnly(size_t Index) noexcept
        {
            --Size;
            const size_t IndexBefore = (Index - HashTableInternal::kGroupWidth) & Capacity;
            const auto EmptyAfter = FGroup(Ctrl + Index).MatchEmpty();
            const auto EmptyBefore = FGroup(Ctrl + IndexBefore).MatchEmpty();

            const bool bWasNeverFull = EmptyBefore && EmptyAfter
                && static_cast<size_t>(EmptyAfter.TrailingZeros() + EmptyBefore.LeadingZeros()) < HashTableInternal::kGroupWidth;

            HashTableInternal::SetCtrl(Ctrl, Capacity, Index,
                bWasNeverFull ? HashTableInternal::CtrlEmpty : HashTableInternal::CtrlDeleted);
            GrowthLeft += bWasNeverFull ? 1 : 0;
        }

        void Resize(size_t NewCapacity)
        {
            LUMINA_CONTAINER_CHECK(HashTableInternal::IsValidCapacity(NewCapacity));

            FCtrl* OldCtrl = Ctrl;
            slot_type* OldSlots = Slots;
            const size_t OldCapacity = Capacity;
            const bool bOldWasHeap = OldCapacity != 0 && !IsInline();

            AllocateBlock(NewCapacity, bOldWasHeap);

            for (size_t Index = 0; Index != OldCapacity; ++Index)
            {
                if (!HashTableInternal::IsFull(OldCtrl[Index]))
                {
                    continue;
                }
                const uint64 Hash = Hasher(TPolicy::Key(OldSlots[Index]));
                const size_t Target = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Capacity).Offset;
                HashTableInternal::SetCtrl(Ctrl, Capacity, Target, HashTableInternal::H2(Hash));
                TPolicy::Transfer(Slots + Target, OldSlots + Index);
            }

            if (bOldWasHeap)
            {
                TAllocator::Deallocate(OldCtrl, HashTableInternal::AllocSize(OldCapacity, sizeof(slot_type), kSlotAlign), kBlockAlign);
            }
        }

        // Shrinking back into the inline block is only safe once the elements have left it.
        void AllocateBlock(size_t NewCapacity, bool bCanReuseInline)
        {
            if constexpr (kInlineCapacity != 0)
            {
                if (bCanReuseInline && NewCapacity <= kInlineCapacity)
                {
                    PointAtBlock(FStorage::GetInlineBlock(), kInlineCapacity);
                    GrowthLeft = HashTableInternal::CapacityToGrowth(Capacity) - Size;
                    return;
                }
            }

            const size_t Bytes = HashTableInternal::AllocSize(NewCapacity, sizeof(slot_type), kSlotAlign);
            PointAtBlock(TAllocator::Allocate(Bytes, kBlockAlign), NewCapacity);
            GrowthLeft = HashTableInternal::CapacityToGrowth(Capacity) - Size;
        }

        /** A table out of growth but only half occupied is full of tombstones, not elements. */
        void RehashAndGrowIfNecessary()
        {
            if (Capacity == 0)
            {
                Resize(1);
                return;
            }

            if (Size <= HashTableInternal::CapacityToGrowth(Capacity) / 2)
            {
                DropDeletesWithoutResize();
                return;
            }

            Resize(Capacity * 2 + 1);
        }

        void DropDeletesWithoutResize()
        {
            LUMINA_CONTAINER_CHECK(HashTableInternal::IsValidCapacity(Capacity));

            alignas(slot_type) unsigned char Raw[sizeof(slot_type)];
            slot_type* Staged = reinterpret_cast<slot_type*>(Raw);

            HashTableInternal::ConvertDeletedToEmptyAndFullToDeleted(Ctrl, Capacity);

            for (size_t Index = 0; Index != Capacity; ++Index)
            {
                if (!HashTableInternal::IsDeleted(Ctrl[Index]))
                {
                    continue;
                }

                const uint64 Hash = Hasher(TPolicy::Key(Slots[Index]));
                const size_t NewIndex = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Capacity).Offset;
                const size_t ProbeOffset = HashTableInternal::FProbeSeq(HashTableInternal::H1(Hash), Capacity).GetOffset();

                const auto ProbeGroupOf = [this, ProbeOffset](size_t Position)
                {
                    return ((Position - ProbeOffset) & Capacity) / HashTableInternal::kGroupWidth;
                };

                if (ProbeGroupOf(NewIndex) == ProbeGroupOf(Index)) [[likely]]
                {
                    HashTableInternal::SetCtrl(Ctrl, Capacity, Index, HashTableInternal::H2(Hash));
                    continue;
                }

                if (HashTableInternal::IsEmpty(Ctrl[NewIndex]))
                {
                    HashTableInternal::SetCtrl(Ctrl, Capacity, NewIndex, HashTableInternal::H2(Hash));
                    TPolicy::Transfer(Slots + NewIndex, Slots + Index);
                    HashTableInternal::SetCtrl(Ctrl, Capacity, Index, HashTableInternal::CtrlEmpty);
                    continue;
                }

                LUMINA_CONTAINER_CHECK(HashTableInternal::IsDeleted(Ctrl[NewIndex]));
                HashTableInternal::SetCtrl(Ctrl, Capacity, NewIndex, HashTableInternal::H2(Hash));

                TPolicy::Transfer(Staged, Slots + Index);
                TPolicy::Transfer(Slots + Index, Slots + NewIndex);
                TPolicy::Transfer(Slots + NewIndex, Staged);
                --Index;
            }

            GrowthLeft = HashTableInternal::CapacityToGrowth(Capacity) - Size;
        }

        FCtrl*     Ctrl  = HashTableInternal::EmptyGroup();
        slot_type* Slots = nullptr;
        size_t     Size = 0;
        size_t     Capacity = 0;
        size_t     GrowthLeft = 0;

        LUMINA_NO_UNIQUE_ADDRESS THasher Hasher;
        LUMINA_NO_UNIQUE_ADDRESS TEqual  Equal;
    };

    template <typename TPolicy,
              typename THasher,
              typename TEqual,
              ContainerAllocatorType TAllocator,
              size_t InlineCapacity>
    class TBasicHashSet : public TRawHashTable<TPolicy, THasher, TEqual, TAllocator, InlineCapacity>
    {
        using Super = TRawHashTable<TPolicy, THasher, TEqual, TAllocator, InlineCapacity>;

    public:

        using Super::Super;

        using key_type   = typename TPolicy::key_type;
        using value_type = typename TPolicy::value_type;

        NODISCARD friend bool operator==(const TBasicHashSet& Left, const TBasicHashSet& Right) noexcept
        {
            if (Left.size() != Right.size())
            {
                return false;
            }
            for (const value_type& Element : Left)
            {
                if (!Right.contains(Element))
                {
                    return false;
                }
            }
            return true;
        }
    };

    template <typename TPolicy,
              typename THasher,
              typename TEqual,
              ContainerAllocatorType TAllocator,
              size_t InlineCapacity>
    class TBasicHashMap : public TRawHashTable<TPolicy, THasher, TEqual, TAllocator, InlineCapacity>
    {
        using Super = TRawHashTable<TPolicy, THasher, TEqual, TAllocator, InlineCapacity>;

    public:

        using Super::Super;

        using key_type    = typename TPolicy::key_type;
        using mapped_type = typename TPolicy::mapped_type;
        using value_type  = typename TPolicy::value_type;
        using iterator    = typename Super::iterator;

        template <typename TKeyArg, typename... TArgs>
        TPair<iterator, bool> try_emplace(TKeyArg&& Key, TArgs&&... Args)
        {
            const TPair<size_t, bool> Placed = this->FindOrPrepareInsert(Key);
            if (Placed.second)
            {
                TPolicy::Construct(this->SlotAt(Placed.first),
                    std::piecewise_construct,
                    std::forward_as_tuple(std::forward<TKeyArg>(Key)),
                    std::forward_as_tuple(std::forward<TArgs>(Args)...));
            }
            return { this->IteratorAt(Placed.first), Placed.second };
        }

        template <typename TKeyArg, typename TValueArg>
        TPair<iterator, bool> insert_or_assign(TKeyArg&& Key, TValueArg&& Value)
        {
            const TPair<size_t, bool> Placed = this->FindOrPrepareInsert(Key);
            if (Placed.second)
            {
                TPolicy::Construct(this->SlotAt(Placed.first),
                    std::piecewise_construct,
                    std::forward_as_tuple(std::forward<TKeyArg>(Key)),
                    std::forward_as_tuple(std::forward<TValueArg>(Value)));
            }
            else
            {
                TPolicy::Value(*this->SlotAt(Placed.first)) = std::forward<TValueArg>(Value);
            }
            return { this->IteratorAt(Placed.first), Placed.second };
        }

        NODISCARD mapped_type& operator[](const key_type& Key) { return try_emplace(Key).first->second; }
        NODISCARD mapped_type& operator[](key_type&& Key) { return try_emplace(std::move(Key)).first->second; }

        NODISCARD mapped_type& at(const key_type& Key)
        {
            const iterator It = this->find(Key);
            LUMINA_CONTAINER_CHECK(It != this->end());
            return It->second;
        }

        NODISCARD const mapped_type& at(const key_type& Key) const
        {
            const typename Super::const_iterator It = this->find(Key);
            LUMINA_CONTAINER_CHECK(It != this->end());
            return It->second;
        }

        NODISCARD friend bool operator==(const TBasicHashMap& Left, const TBasicHashMap& Right) noexcept
        {
            if (Left.size() != Right.size())
            {
                return false;
            }
            for (const auto& Pair : Left)
            {
                const auto It = Right.find(Pair.first);
                if (It == Right.end() || !(It->second == Pair.second))
                {
                    return false;
                }
            }
            return true;
        }
    };

    /** Elements live in the table, so a rehash moves them and no reference into one survives an insert. */
    template <typename T,
              typename THasher = FDefaultHash,
              typename TEqual = FDefaultEqual,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using THashSet = TBasicHashSet<TSetPolicy<T>, THasher, TEqual, TAllocator, 0>;

    template <typename K,
              typename V,
              typename THasher = FDefaultHash,
              typename TEqual = FDefaultEqual,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using THashMap = TBasicHashMap<TMapPolicy<K, V>, THasher, TEqual, TAllocator, 0>;

    /** Elements are separately allocated, so an element address stays valid for as long as the element does. */
    template <typename T,
              typename THasher = FDefaultHash,
              typename TEqual = FDefaultEqual,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using TNodeHashSet = TBasicHashSet<TNodeSetPolicy<T, TAllocator>, THasher, TEqual, TAllocator, 0>;

    template <typename K,
              typename V,
              typename THasher = FDefaultHash,
              typename TEqual = FDefaultEqual,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using TNodeHashMap = TBasicHashMap<TNodeMapPolicy<K, V, TAllocator>, THasher, TEqual, TAllocator, 0>;

    /** Carries room for N entries inline; once it outgrows them it moves to the heap and stays there. */
    template <typename T,
              size_t N,
              typename THasher = FDefaultHash,
              typename TEqual = FDefaultEqual,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using TInlineHashSet = TBasicHashSet<TSetPolicy<T>, THasher, TEqual, TAllocator, N>;

    template <typename K,
              typename V,
              size_t N,
              typename THasher = FDefaultHash,
              typename TEqual = FDefaultEqual,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using TInlineHashMap = TBasicHashMap<TMapPolicy<K, V>, THasher, TEqual, TAllocator, N>;

    template <typename TPolicy, typename THasher, typename TEqual, ContainerAllocatorType TAllocator, size_t N, typename TPredicate>
    size_t erase_if(TRawHashTable<TPolicy, THasher, TEqual, TAllocator, N>& Table, TPredicate Predicate)
    {
        size_t Removed = 0;
        for (auto It = Table.begin(); It != Table.end();)
        {
            if (Predicate(*It))
            {
                It = Table.erase(It);
                ++Removed;
            }
            else
            {
                ++It;
            }
        }
        return Removed;
    }

    template <typename K, typename V>
    using TScratchHashMap = TBasicHashMap<TMapPolicy<K, V>, FDefaultHash, FDefaultEqual, FScratchAllocator, 0>;

    template <typename T>
    using TScratchHashSet = TBasicHashSet<TSetPolicy<T>, FDefaultHash, FDefaultEqual, FScratchAllocator, 0>;

    template <typename TPolicy, typename THasher, typename TEqual, ContainerAllocatorType TAllocator, size_t InlineCapacity>
    FORCEINLINE void swap(TRawHashTable<TPolicy, THasher, TEqual, TAllocator, InlineCapacity>& Left,
                          TRawHashTable<TPolicy, THasher, TEqual, TAllocator, InlineCapacity>& Right) noexcept
    {
        Left.swap(Right);
    }
}

namespace Lumina
{
    template <typename K, typename V,
              typename H = Containers::FDefaultHash,
              typename E = Containers::FDefaultEqual>
    using THashMap = Containers::THashMap<K, V, H, E>;

    template <typename T,
              typename H = Containers::FDefaultHash,
              typename E = Containers::FDefaultEqual>
    using THashSet = Containers::THashSet<T, H, E>;

    template <typename K, typename V, size_t NodeCount,
              size_t BucketCount = NodeCount + 1,
              bool bEnableOverflow = true,
              typename H = Containers::FDefaultHash,
              typename E = Containers::FDefaultEqual>
    using TFixedHashMap = Containers::TInlineHashMap<K, V, NodeCount, H, E>;

    template <typename T, size_t NodeCount,
              size_t BucketCount = NodeCount + 1,
              bool bEnableOverflow = true,
              typename H = Containers::FDefaultHash,
              typename E = Containers::FDefaultEqual>
    using TFixedHashSet = Containers::TInlineHashSet<T, NodeCount, H, E>;
}
