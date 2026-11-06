#pragma once

#include <bit>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "Memory/Construct.h"
#include "Core/Templates/IntegerCompare.h"
#include "HashPrimitives.h"
#include "Pair.h"
#include "Vector.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define LUMINA_HASHTABLE_SSE2 1
    #include <emmintrin.h>
#else
    #define LUMINA_HASHTABLE_SSE2 0
#endif

namespace Lumina::Containers
{
    // Hashes anything with a GetTypeHash found by ADL, which is what makes lookup heterogeneous by default.
    struct FDefaultHash
    {
        using is_transparent = void;

        // Never re-mixed: a heterogeneous lookup only finds a key when both spellings hash identically.
        template <typename T>
        NODISCARD FORCEINLINE static uint64 operator()(const T& Key) noexcept
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
        NODISCARD FORCEINLINE static bool operator()(const TLeft& Left, const TRight& Right) noexcept
        {
            if constexpr (ValueComparableInteger<TLeft> && ValueComparableInteger<TRight>)
            {
                return Cmp::Equal(Left, Right);
            }
            else
            {
                return Left == Right;
            }
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

    // H1 picks the group, H2 is the 7 bits stored in the control byte and matched a group at a time.
    NODISCARD FORCEINLINE constexpr size_t H1(uint64 Hash) noexcept { return Hash >> 7; }
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

        // Empty and deleted are the only bytes below the sentinel, so one signed compare separates them from full.
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

    // Byte-parallel fallback for any target without SSE2; half the group width, identical semantics.
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

    NODISCARD FORCEINLINE constexpr bool IsValidCapacity(size_t Cap) noexcept
    {
        return Cap > 0 && ((Cap + 1) & Cap) == 0;
    }

    NODISCARD FORCEINLINE constexpr size_t NormalizeCapacity(size_t Count) noexcept
    {
        return Count == 0 ? 1 : (~size_t(0) >> std::countl_zero(Count));
    }

    // The table grows at 7/8, which is the load the group probe absorbs without long chains.
    NODISCARD FORCEINLINE constexpr size_t CapacityToGrowth(size_t Cap) noexcept
    {
        return Cap - Cap / 8;
    }

    NODISCARD FORCEINLINE constexpr size_t GrowthToLowerBoundCapacity(size_t Growth) noexcept
    {
        return Growth + static_cast<size_t>((static_cast<int64>(Growth) - 1) / 7);
    }

    NODISCARD FORCEINLINE constexpr size_t NumCtrlBytes(size_t Cap) noexcept
    {
        return Cap + 1 + kGroupWidth;
    }

    NODISCARD FORCEINLINE constexpr size_t SlotOffset(size_t Cap, size_t SlotAlign) noexcept
    {
        return (NumCtrlBytes(Cap) + SlotAlign - 1) & ~(SlotAlign - 1);
    }

    NODISCARD FORCEINLINE constexpr size_t AllocSize(size_t Cap, size_t SlotSize, size_t SlotAlign) noexcept
    {
        return SlotOffset(Cap, SlotAlign) + Cap * SlotSize;
    }

    // A capacity-0 table probes this instead of branching on null; the leading sentinel stops every search.
    alignas(16) inline constexpr FCtrl kEmptyGroup[] =
    {
        CtrlSentinel, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty,
        CtrlEmpty,    CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty, CtrlEmpty,
    };

    // Not a static local, which a constexpr function only allows on compilers with P2647.
    NODISCARD FORCEINLINE constexpr FCtrl* EmptyGroup() noexcept
    {
        return const_cast<FCtrl*>(kEmptyGroup);
    }

    inline void ResetCtrl(FCtrl* Ctrl, size_t Cap) noexcept
    {
        std::memset(Ctrl, CtrlEmpty, NumCtrlBytes(Cap));
        Ctrl[Cap] = CtrlSentinel;
    }

    // The mirrored write keeps the cloned tail bytes in step, so a group load near the end stays valid.
    FORCEINLINE void SetCtrl(FCtrl* Ctrl, size_t Cap, size_t Index, FCtrl Value) noexcept
    {
        Ctrl[Index] = Value;
        Ctrl[((Index - kGroupWidth) & Cap) + 1 + ((kGroupWidth - 1) & Cap)] = Value;
    }

    inline void ConvertDeletedToEmptyAndFullToDeleted(FCtrl* Ctrl, size_t Cap) noexcept
    {
        for (FCtrl* Position = Ctrl; Position < Ctrl + Cap; Position += kGroupWidth)
        {
            FGroup(Position).ConvertSpecialToEmptyAndFullToDeleted(Position);
        }
        std::memcpy(Ctrl + Cap + 1, Ctrl, kGroupWidth);
        Ctrl[Cap] = CtrlSentinel;
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

        // Triangular steps visit every group exactly once when the capacity is a power of two minus one.
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

    inline FFindInfo FindFirstNonFull(const FCtrl* Ctrl, uint64 Hash, size_t Cap) noexcept
    {
        FProbeSeq Seq(H1(Hash), Cap);
        while (true)
        {
            const FGroup Group(Ctrl + Seq.GetOffset());
            if (const auto Mask = Group.MatchEmptyOrDeleted())
            {
                return { Seq.GetOffset(*Mask), Seq.GetIndex() };
            }
            Seq.Next();
            LUMINA_CONTAINER_CHECK_WITHIN(Seq.GetIndex(), Cap);
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
            Memory::ConstructAt(Destination, std::move(*Source));
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

    // Empty base, so a table with no inline capacity stays at five words.
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
        static constexpr bool bBitwiseCopy    = std::is_trivially_copyable_v<T>;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return Slot; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return Slot; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return Slot; }

        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            Memory::ConstructAt(Slot, std::forward<TArgs>(Args)...);
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

        // The caller sees a const key, which cannot be moved; the mutable twin is what a rehash relocates.
        union FMapSlot
        {
            TPair<const K, V> Value;
            FMutablePair      Mutable;

            FMapSlot() noexcept {}
            ~FMapSlot() noexcept {}
            FMapSlot(const FMapSlot&) = delete;
            FMapSlot& operator=(const FMapSlot&) = delete;
            FMapSlot(FMapSlot&&) = delete;
            FMapSlot& operator=(FMapSlot&&) = delete;
        };

        using slot_type   = FMapSlot;
        using key_type    = K;
        using value_type  = TPair<const K, V>;
        using init_type   = TPair<K, V>;
        using mapped_type = V;

        static constexpr bool bTrivialDestroy = std::is_trivially_destructible_v<FMutablePair>;
        static constexpr bool bBitwiseCopy    = std::is_trivially_copyable_v<FMutablePair>;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return Slot.Value.Key; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return Slot.Value; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return Slot.Value; }
        NODISCARD FORCEINLINE static V& Value(slot_type& Slot) noexcept { return Slot.Mutable.Value; }

        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            Memory::ConstructAt(&Slot->Mutable, std::forward<TArgs>(Args)...);
        }

        FORCEINLINE static void Destroy(slot_type* Slot) noexcept { Slot->Mutable.~FMutablePair(); }

        FORCEINLINE static void Transfer(slot_type* Destination, slot_type* Source) noexcept
        {
            HashTableInternal::RelocateOne(&Destination->Mutable, &Source->Mutable);
        }
    };

    // Slot is a pointer to a separately allocated element, so an element address survives every rehash.
    template <typename T, ContainerAllocatorType TAllocator>
    struct TNodeSetPolicy
    {
        using slot_type   = T*;
        using key_type    = T;
        using value_type  = T;
        using init_type   = T;
        using mapped_type = void;

        static constexpr bool bTrivialDestroy = false;
        static constexpr bool bBitwiseCopy    = false;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return *Slot; }

        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            void* Block = TAllocator::Allocate(sizeof(T), alignof(T));
            *Slot = Memory::ConstructAt(static_cast<T*>(Block), std::forward<TArgs>(Args)...);
        }

        FORCEINLINE static void Destroy(slot_type* Slot) noexcept
        {
            (*Slot)->~T();
            TAllocator::Deallocate(*Slot, sizeof(T), alignof(T));
            *Slot = nullptr;
        }

        // Only the pointer moves, which is exactly why the element address is stable.
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
        static constexpr bool bBitwiseCopy    = false;

        NODISCARD FORCEINLINE static const key_type& Key(const slot_type& Slot) noexcept { return Slot->Key; }
        NODISCARD FORCEINLINE static value_type& Element(slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static const value_type& Element(const slot_type& Slot) noexcept { return *Slot; }
        NODISCARD FORCEINLINE static V& Value(slot_type& Slot) noexcept { return Slot->Value; }

        // The node never moves, so the key can stay const here and no mutable twin is needed.
        template <typename... TArgs>
        FORCEINLINE static void Construct(slot_type* Slot, TArgs&&... Args)
        {
            void* Block = TAllocator::Allocate(sizeof(FNode), alignof(FNode));
            *Slot = Memory::ConstructAt(static_cast<FNode*>(Block), std::forward<TArgs>(Args)...);
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

    // Open-addressed table with one control byte per slot, probed a whole group at a time.
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

        // Below 2^32 - 1, so doubling the last capacity or adding one to it never wraps the uint32 it is stored in.
        static constexpr size_t kMaxCapacity = 0x7FFFFFFFu;

    public:

        using policy_type     = TPolicy;
        using slot_type       = typename TPolicy::slot_type;
        using key_type        = typename TPolicy::key_type;
        using value_type      = typename TPolicy::value_type;
        using init_type       = typename TPolicy::init_type;
        using size_type       = int32;
        using difference_type = ptrdiff_t;
        using hasher          = THasher;
        using key_equal       = TEqual;
        using allocator_type  = TAllocator;
        using reference       = value_type&;
        using const_reference = const value_type&;
        using pointer         = value_type*;
        using const_pointer   = const value_type*;
        using mapped_type     = typename TPolicy::mapped_type;

        static constexpr size_type InlineCapacityV = static_cast<size_type>(kInlineCapacity);

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

        // Constant-initializable, so a global table can be constinit and no static initializer can run before it.
        constexpr TRawHashTable() noexcept { ResetToInline(); }

        explicit TRawHashTable(size_type InitialCapacity)
        {
            ResetToInline();
            Reserve(InitialCapacity);
        }

        TRawHashTable(const TRawHashTable& Other)
            : Hasher(Other.Hasher)
            , Equal(Other.Equal)
        {
            ResetToInline();
            CopyElementsFrom(Other);
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
            Reserve(static_cast<int32>(Values.size()));
            for (const init_type& Value : Values)
            {
                InsertValue(Value);
            }
        }

        ~TRawHashTable() { ReleaseBlock(); }

        TRawHashTable& operator=(const TRawHashTable& Other)
        {
            if (this != &Other && OwnsRegion(&Other))
            {
                TRawHashTable Copy(Other);
                *this = std::move(Copy);
            }
            else if (this != &Other)
            {
                Reset();
                Hasher = Other.Hasher;
                Equal  = Other.Equal;
                CopyElementsFrom(Other);
            }
            return *this;
        }

        TRawHashTable& operator=(TRawHashTable&& Other) noexcept
        {
            if (this != &Other)
            {
                // Other is emptied before our values go, since it may live inside one of them.
                TRawHashTable Taken(std::move(Other));
                ReleaseBlock();
                Hasher = std::move(Taken.Hasher);
                Equal  = std::move(Taken.Equal);
                AdoptFrom(Taken);
            }
            return *this;
        }

        NODISCARD FORCEINLINE int32 Num() const noexcept { return static_cast<int32>(ElementCount); }
        NODISCARD FORCEINLINE int32 Max() const noexcept { return static_cast<int32>(Cap); }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return ElementCount == 0; }

        // The first element at or after slot Cursor, leaving Cursor past it, so a walk resumes across calls and survives a rehash.
        NODISCARD value_type* FindNextOccupied(int32& Cursor) noexcept
        {
            for (size_t Slot = static_cast<uint32>(Cursor); Slot < Cap; ++Slot)
            {
                if (HashTableInternal::IsFull(Ctrl[Slot]))
                {
                    Cursor = static_cast<int32>(Slot + 1);
                    return &TPolicy::Element(Slots[Slot]);
                }
            }
            Cursor = static_cast<int32>(Cap);
            return nullptr;
        }

        NODISCARD FORCEINLINE float LoadFactor() const noexcept
        {
            return Cap == 0 ? 0.0f : static_cast<float>(ElementCount) / static_cast<float>(Cap);
        }

        template <typename TSelf>
        NODISCARD auto begin(this TSelf&& Self) noexcept
        {
            TIterator<std::is_const_v<std::remove_reference_t<TSelf>>> It(Self.Ctrl, Self.Slots);
            It.SkipEmptyOrDeleted();
            return It;
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto end(this TSelf&&) noexcept { return TIterator<std::is_const_v<std::remove_reference_t<TSelf>>>(); }

        // Walks the table with the option to remove the element it stands on, as Unreal's CreateIterator does.
        template <bool bConst>
        class TCursor
        {
            using FTable = std::conditional_t<bConst, const TRawHashTable, TRawHashTable>;
            using FIterator = std::conditional_t<bConst, const_iterator, iterator>;

        public:

            explicit TCursor(FTable& InTable) noexcept : Table(&InTable), It(InTable.begin()) {}

            NODISCARD explicit operator bool() const noexcept { return It != Table->end(); }

            TCursor& operator++() noexcept
            {
                if (bRemoved)
                {
                    bRemoved = false;
                }
                else
                {
                    ++It;
                }
                return *this;
            }

            NODISCARD decltype(auto) operator*() const noexcept { return *It; }
            NODISCARD auto operator->() const noexcept { return It.operator->(); }

            void RemoveCurrent() noexcept requires (!bConst)
            {
                It = Table->EraseIterator(It);
                bRemoved = true;
            }

        private:

            FTable*   Table;
            FIterator It;
            bool      bRemoved = false;
        };

        NODISCARD TCursor<false> CreateIterator() noexcept { return TCursor<false>(*this); }
        NODISCARD TCursor<true> CreateConstIterator() const noexcept { return TCursor<true>(*this); }

        // Drops every element but keeps the block, so refilling it does not rehash.
        void Reset() noexcept
        {
            DestroyAllSlots();
            ElementCount = 0;
            if (Cap != 0)
            {
                HashTableInternal::ResetCtrl(Ctrl, Cap);
                GrowthLeft = static_cast<uint32>(HashTableInternal::CapacityToGrowth(Cap));
            }
        }

        // Drops every element and the block, keeping room for Slack entries.
        void Empty(int32 Slack = 0)
        {
            if (Slack > 0 && !IsInline() && Cap == CapacityFor(static_cast<uint32>(Slack)))
            {
                Reset();
                return;
            }
            ReleaseBlock();
            if (Slack > 0)
            {
                Reserve(Slack);
            }
        }

        void Reserve(int32 InCount)
        {
            const uint32 Count = static_cast<uint32>(Private::CheckCapacity(static_cast<uint32>(InCount), HashTableInternal::CapacityToGrowth(kMaxCapacity)));
            if (Count > static_cast<uint64>(ElementCount) + GrowthLeft)
            {
                Resize(CapacityFor(Count));
            }
        }

        void Shrink() { Rehash(0); }

        template <typename TKeyArg>
        NODISCARD FORCEINLINE bool Contains(const TKeyArg& Key) const noexcept
        {
            return FindIndex(Key, Hasher(Key)) != kNoIndex;
        }

        template <typename TKeyArg>
        requires (!std::is_convertible_v<const TKeyArg&, const_iterator>)
        int32 Remove(const TKeyArg& Key) noexcept
        {
            const size_t Index = FindIndex(Key, Hasher(Key));
            if (Index == kNoIndex)
            {
                return 0;
            }
            EraseAt(Index);
            return 1;
        }

        // Removes the element Position stands on and hands back the iterator to the one after it.
        iterator Remove(const_iterator Position) noexcept { return EraseIterator(Position); }

        template <typename TPredicate>
        int32 RemoveAll(TPredicate&& Predicate)
        {
            int32 Removed = 0;
            for (auto It = begin(); It != end();)
            {
                if (Predicate(*It))
                {
                    It = EraseIterator(It);
                    ++Removed;
                }
                else
                {
                    ++It;
                }
            }
            return Removed;
        }

        // Looks up with a caller-supplied hash and equality, for a key type the table is not built on.
        template <typename TKeyArg, typename THashFn, typename TEqualFn, typename TSelf>
        NODISCARD auto FindAs(this TSelf&& Self, const TKeyArg& Key, THashFn&& HashFn, TEqualFn&& EqualFn) noexcept -> TLikeSelf<TSelf, value_type>*
        {
            const size_t Index = Self.FindIndexWith(Key, static_cast<uint64>(HashFn(Key)), EqualFn);
            return Index == kNoIndex ? nullptr : &TPolicy::Element(Self.Slots[Index]);
        }

        friend void swap(TRawHashTable& Left, TRawHashTable& Right) noexcept
        {
            Left.SwapWith(Right);
        }

        // True while the table still lives in its inline block rather than a heap allocation.
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

        NODISCARD hasher GetHasher() const { return Hasher; }
        NODISCARD key_equal GetKeyEqual() const { return Equal; }

        // Bytes held by the table, so a memory report can attribute them without guessing.
        NODISCARD size_t GetAllocatedSize() const noexcept
        {
            return (Cap == 0 || IsInline()) ? 0 : HashTableInternal::AllocSize(Cap, sizeof(slot_type), kSlotAlign);
        }

    protected:

        template <typename TKeyArg, typename TSelf>
        NODISCARD FORCEINLINE auto FindElement(this TSelf&& Self, const TKeyArg& Key) noexcept -> TLikeSelf<TSelf, value_type>*
        {
            const size_t Index = Self.FindIndex(Key, Self.Hasher(Key));
            return Index == kNoIndex ? nullptr : &TPolicy::Element(Self.Slots[Index]);
        }

        template <typename TKeyArg, typename TSelf>
        NODISCARD auto FindIt(this TSelf&& Self, const TKeyArg& Key) noexcept
        {
            using FIterator = TIterator<std::is_const_v<std::remove_reference_t<TSelf>>>;
            const size_t Index = Self.FindIndex(Key, Self.Hasher(Key));
            return Index == kNoIndex ? FIterator() : FIterator(Self.Ctrl + Index, Self.Slots + Index);
        }

        // Staging the element first is what lets an arbitrary argument pack find its own key.
        template <typename... TArgs>
        TPair<iterator, bool> EmplaceElement(TArgs&&... Args)
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

        iterator EraseIterator(const_iterator Position) noexcept
        {
            LUMINA_CONTAINER_CHECK(Position.Ctrl != nullptr);
            const size_t Index = static_cast<size_t>(Position.Ctrl - Ctrl);
            EraseAt(Index);

            iterator Next(Ctrl + Index, Slots + Index);
            Next.SkipEmptyOrDeleted();
            return Next;
        }

        void Rehash(int32 InCount)
        {
            const uint32 Count = static_cast<uint32>(Private::CheckCapacity(static_cast<uint32>(InCount), kMaxCapacity));
            if (Count == 0 && ElementCount == 0)
            {
                ReleaseBlock();
                return;
            }

            const size_t LowerBound = HashTableInternal::GrowthToLowerBoundCapacity(ElementCount);
            const size_t NewCapacity = HashTableInternal::NormalizeCapacity(Count < LowerBound ? LowerBound : static_cast<size_t>(Count));
            if (NewCapacity != Cap)
            {
                Resize(NewCapacity);
            }
        }

        void SwapWith(TRawHashTable& Other) noexcept
        {
            if (this == &Other)
            {
                return;
            }

            if constexpr (kInlineCapacity == 0)
            {
                std::swap(Ctrl, Other.Ctrl);
                std::swap(Slots, Other.Slots);
                std::swap(ElementCount, Other.ElementCount);
                std::swap(Cap, Other.Cap);
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

    protected:

        template <typename TKeyArg>
        NODISCARD size_t FindIndex(const TKeyArg& Key, uint64 Hash) const noexcept
        {
            HashTableInternal::FProbeSeq Seq(HashTableInternal::H1(Hash), Cap);
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
                LUMINA_CONTAINER_CHECK_WITHIN(Seq.GetIndex(), Cap);
            }
        }

        template <typename TKeyArg, typename TEqualFn>
        NODISCARD size_t FindIndexWith(const TKeyArg& Key, uint64 Hash, TEqualFn& EqualFn) const noexcept
        {
            HashTableInternal::FProbeSeq Seq(HashTableInternal::H1(Hash), Cap);
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
                LUMINA_CONTAINER_CHECK_WITHIN(Seq.GetIndex(), Cap);
            }
        }

        // An insert now may rehash, which moves every element, so a caller's reference into the table goes stale.
        NODISCARD FORCEINLINE bool MayRehashOnInsert() const noexcept { return GrowthLeft == 0; }

        NODISCARD FORCEINLINE bool OwnsRegion(const void* Address) const noexcept
        {
            const uint8* Byte = static_cast<const uint8*>(Address);
            return Slots != nullptr && Byte >= reinterpret_cast<const uint8*>(Slots) && Byte < reinterpret_cast<const uint8*>(Slots + Cap);
        }

        size_t PrepareInsert(uint64 Hash)
        {
            HashTableInternal::FFindInfo Target = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Cap);
            if (GrowthLeft == 0 && !HashTableInternal::IsDeleted(Ctrl[Target.Offset]))
            {
                RehashAndGrowIfNecessary();
                Target = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Cap);
            }

            ++ElementCount;
            GrowthLeft -= HashTableInternal::IsEmpty(Ctrl[Target.Offset]) ? 1 : 0;
            HashTableInternal::SetCtrl(Ctrl, Cap, Target.Offset, HashTableInternal::H2(Hash));
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

        // Finds Key, or claims a slot and reports that the caller still has to build the element in it.
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
                return (Value.Key);
            }
        }

        NODISCARD static size_t CapacityFor(uint32 Count) noexcept
        {
            return HashTableInternal::NormalizeCapacity(HashTableInternal::GrowthToLowerBoundCapacity(Count));
        }

        // At the source's capacity every element keeps its slot, so nothing hashes; a source over twice too big is compacted instead.
        void CopyElementsFrom(const TRawHashTable& Other)
        {
            LUMINA_CONTAINER_CHECK(ElementCount == 0);
            if (Other.ElementCount == 0)
            {
                return;
            }

            const size_t Fit = CapacityFor(Other.ElementCount);
            if (Other.Cap > Fit * 2 + 1)
            {
                if (Cap < Fit)
                {
                    Resize(Fit);
                }
                for (size_t Index = 0, Remaining = Other.ElementCount; Remaining != 0; ++Index)
                {
                    if (HashTableInternal::IsFull(Other.Ctrl[Index]))
                    {
                        --Remaining;
                        const size_t Target = PrepareInsert(Hasher(TPolicy::Key(Other.Slots[Index])));
                        TPolicy::Construct(Slots + Target, TPolicy::Element(Other.Slots[Index]));
                    }
                }
                return;
            }

            if (Cap != Other.Cap)
            {
                ReleaseBlock();
                if (Cap != Other.Cap)
                {
                    AllocateBlock(Other.Cap, false);
                }
            }

            if constexpr (TPolicy::bBitwiseCopy)
            {
                std::memcpy(static_cast<void*>(Slots), static_cast<const void*>(Other.Slots), static_cast<size_t>(Cap) * sizeof(slot_type));
            }
            else
            {
                for (size_t Index = 0, Remaining = Other.ElementCount; Remaining != 0; ++Index)
                {
                    if (HashTableInternal::IsFull(Other.Ctrl[Index]))
                    {
                        --Remaining;
                        TPolicy::Construct(Slots + Index, TPolicy::Element(Other.Slots[Index]));
                    }
                }
            }

            // Control bytes last, so a copy that throws leaves a table that owns nothing it did not build.
            std::memcpy(Ctrl, Other.Ctrl, HashTableInternal::NumCtrlBytes(Cap));
            ElementCount   = Other.ElementCount;
            GrowthLeft     = Other.GrowthLeft;
        }

        constexpr void AdoptEmpty() noexcept
        {
            Ctrl           = HashTableInternal::EmptyGroup();
            Slots          = nullptr;
            ElementCount   = 0;
            Cap            = 0;
            GrowthLeft     = 0;
        }

        constexpr void ResetToInline() noexcept
        {
            if constexpr (kInlineCapacity == 0)
            {
                AdoptEmpty();
            }
            else
            {
                PointAtBlock(FStorage::GetInlineBlock(), kInlineCapacity);
                ElementCount = 0;
                GrowthLeft = static_cast<uint32>(HashTableInternal::CapacityToGrowth(kInlineCapacity));
            }
        }

        FORCEINLINE void PointAtBlock(void* Block, size_t NewCapacity) noexcept
        {
            Ctrl = static_cast<FCtrl*>(Block);
            Slots = reinterpret_cast<slot_type*>(static_cast<uint8*>(Block) + HashTableInternal::SlotOffset(NewCapacity, kSlotAlign));
            Cap = static_cast<uint32>(NewCapacity);
            HashTableInternal::ResetCtrl(Ctrl, Cap);
        }

        // A heap table hands over its pointers; an inline one has to move its elements slot for slot.
        void AdoptFrom(TRawHashTable& Other) noexcept
        {
            if (!Other.IsInline())
            {
                ReleaseBlock();
                Ctrl           = Other.Ctrl;
                Slots          = Other.Slots;
                ElementCount   = Other.ElementCount;
                Cap            = Other.Cap;
                GrowthLeft     = Other.GrowthLeft;
                Other.ResetToInline();
                return;
            }

            if constexpr (kInlineCapacity != 0)
            {
                ResetToInline();

                // Both blocks have the same capacity, so every element keeps its slot and no rehash is needed.
                std::memcpy(Ctrl, Other.Ctrl, HashTableInternal::NumCtrlBytes(Cap));
                for (size_t Index = 0; Index != Cap; ++Index)
                {
                    if (HashTableInternal::IsFull(Ctrl[Index]))
                    {
                        TPolicy::Transfer(Slots + Index, Other.Slots + Index);
                    }
                }
                ElementCount = Other.ElementCount;
                GrowthLeft = Other.GrowthLeft;

                Other.ElementCount = 0;
                HashTableInternal::ResetCtrl(Other.Ctrl, Other.Cap);
                Other.GrowthLeft = static_cast<uint32>(HashTableInternal::CapacityToGrowth(Other.Cap));
            }
        }

        void DestroyAllSlots() noexcept
        {
            if (Cap == 0 || ElementCount == 0)
            {
                return;
            }
            if constexpr (!TPolicy::bTrivialDestroy)
            {
                for (size_t Index = 0, Remaining = ElementCount; Remaining != 0; ++Index)
                {
                    if (HashTableInternal::IsFull(Ctrl[Index]))
                    {
                        TPolicy::Destroy(Slots + Index);
                        --Remaining;
                    }
                }
            }
        }

        void ReleaseBlock() noexcept
        {
            if (Cap == 0)
            {
                return;
            }
            DestroyAllSlots();
            if (!IsInline())
            {
                TAllocator::Deallocate(Ctrl, HashTableInternal::AllocSize(Cap, sizeof(slot_type), kSlotAlign), kBlockAlign);
            }
            ResetToInline();
        }

        // An erase whose group still holds an empty byte can clear the slot outright, since no probe ran past it.
        void EraseMetaOnly(size_t Index) noexcept
        {
            --ElementCount;
            const size_t IndexBefore = (Index - HashTableInternal::kGroupWidth) & Cap;
            const auto EmptyAfter = FGroup(Ctrl + Index).MatchEmpty();
            const auto EmptyBefore = FGroup(Ctrl + IndexBefore).MatchEmpty();

            const bool bWasNeverFull = EmptyBefore && EmptyAfter
                && static_cast<size_t>(EmptyAfter.TrailingZeros() + EmptyBefore.LeadingZeros()) < HashTableInternal::kGroupWidth;

            HashTableInternal::SetCtrl(Ctrl, Cap, Index,
                bWasNeverFull ? HashTableInternal::CtrlEmpty : HashTableInternal::CtrlDeleted);
            GrowthLeft += bWasNeverFull ? 1 : 0;
        }

        void Resize(size_t NewCapacity)
        {
            LUMINA_CONTAINER_CHECK(HashTableInternal::IsValidCapacity(NewCapacity));
            (void)Private::CheckCapacity(NewCapacity, kMaxCapacity);

            FCtrl* OldCtrl = Ctrl;
            slot_type* OldSlots = Slots;
            const size_t OldCapacity = Cap;
            const bool bOldWasHeap = OldCapacity != 0 && !IsInline();

            AllocateBlock(NewCapacity, bOldWasHeap);

            for (size_t Index = 0, Remaining = static_cast<size_t>(ElementCount); Remaining != 0; ++Index)
            {
                if (!HashTableInternal::IsFull(OldCtrl[Index]))
                {
                    continue;
                }
                --Remaining;
                const uint64 Hash = Hasher(TPolicy::Key(OldSlots[Index]));
                const size_t Target = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Cap).Offset;
                HashTableInternal::SetCtrl(Ctrl, Cap, Target, HashTableInternal::H2(Hash));
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
                    GrowthLeft = static_cast<uint32>(HashTableInternal::CapacityToGrowth(Cap) - ElementCount);
                    return;
                }
            }

            const size_t Bytes = HashTableInternal::AllocSize(NewCapacity, sizeof(slot_type), kSlotAlign);
            PointAtBlock(TAllocator::Allocate(Bytes, kBlockAlign), NewCapacity);
            GrowthLeft = static_cast<uint32>(HashTableInternal::CapacityToGrowth(Cap) - ElementCount);
        }

        // Under half full it clears tombstones in place, else it doubles: twice as fast under churn as abseil's 25/32, as measured.
        void RehashAndGrowIfNecessary()
        {
            if (Cap == 0)
            {
                Resize(1);
                return;
            }

            if (ElementCount <= HashTableInternal::CapacityToGrowth(Cap) / 2)
            {
                DropDeletesWithoutResize();
                return;
            }

            Resize(static_cast<size_t>(Cap) * 2 + 1);
        }

        void DropDeletesWithoutResize()
        {
            LUMINA_CONTAINER_CHECK(HashTableInternal::IsValidCapacity(Cap));

            alignas(slot_type) unsigned char Raw[sizeof(slot_type)];
            slot_type* Staged = reinterpret_cast<slot_type*>(Raw);

            HashTableInternal::ConvertDeletedToEmptyAndFullToDeleted(Ctrl, Cap);

            for (size_t Index = 0; Index != Cap; ++Index)
            {
                if (!HashTableInternal::IsDeleted(Ctrl[Index]))
                {
                    continue;
                }

                const uint64 Hash = Hasher(TPolicy::Key(Slots[Index]));
                const size_t NewIndex = HashTableInternal::FindFirstNonFull(Ctrl, Hash, Cap).Offset;
                const size_t ProbeOffset = HashTableInternal::FProbeSeq(HashTableInternal::H1(Hash), Cap).GetOffset();

                const auto ProbeGroupOf = [this, ProbeOffset](size_t Position)
                {
                    return ((Position - ProbeOffset) & Cap) / HashTableInternal::kGroupWidth;
                };

                if (ProbeGroupOf(NewIndex) == ProbeGroupOf(Index)) [[likely]]
                {
                    HashTableInternal::SetCtrl(Ctrl, Cap, Index, HashTableInternal::H2(Hash));
                    continue;
                }

                if (HashTableInternal::IsEmpty(Ctrl[NewIndex]))
                {
                    HashTableInternal::SetCtrl(Ctrl, Cap, NewIndex, HashTableInternal::H2(Hash));
                    TPolicy::Transfer(Slots + NewIndex, Slots + Index);
                    HashTableInternal::SetCtrl(Ctrl, Cap, Index, HashTableInternal::CtrlEmpty);
                    continue;
                }

                LUMINA_CONTAINER_CHECK(HashTableInternal::IsDeleted(Ctrl[NewIndex]));
                HashTableInternal::SetCtrl(Ctrl, Cap, NewIndex, HashTableInternal::H2(Hash));

                TPolicy::Transfer(Staged, Slots + Index);
                TPolicy::Transfer(Slots + Index, Slots + NewIndex);
                TPolicy::Transfer(Slots + NewIndex, Staged);
                --Index;
            }

            GrowthLeft = static_cast<uint32>(HashTableInternal::CapacityToGrowth(Cap) - ElementCount);
        }

        FCtrl*     Ctrl           = HashTableInternal::EmptyGroup();
        slot_type* Slots          = nullptr;
        uint32     ElementCount   = 0;
        uint32     Cap            = 0;
        uint32     GrowthLeft     = 0;

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

        using key_type    = typename TPolicy::key_type;
        using value_type  = typename TPolicy::value_type;
        using ElementType = value_type;

        const value_type& Add(const value_type& Value) { return *this->InsertValue(Value).Key; }
        const value_type& Add(value_type&& Value) { return *this->InsertValue(std::move(Value)).Key; }

        // Adds the element unless an equal one is present, and says which happened.
        bool TryAdd(const value_type& Value) { return this->InsertValue(Value).Value; }
        bool TryAdd(value_type&& Value) { return this->InsertValue(std::move(Value)).Value; }

        template <typename TArg>
        requires (!std::is_same_v<std::remove_cvref_t<TArg>, value_type> && std::is_constructible_v<value_type, TArg&&>)
        bool TryAdd(TArg&& Arg) { return this->EmplaceElement(std::forward<TArg>(Arg)).Value; }

        template <typename... TArgs>
        const value_type& Emplace(TArgs&&... Args) { return *this->EmplaceElement(std::forward<TArgs>(Args)...).Key; }

        template <typename TKeyArg>
        NODISCARD FORCEINLINE const value_type* Find(const TKeyArg& Key) const noexcept
        {
            return this->FindElement(Key);
        }

        template <typename TRange>
        void Append(const TRange& Range)
        {
            for (const auto& Value : Range)
            {
                Add(Value);
            }
        }

        void Append(std::initializer_list<value_type> Values)
        {
            this->Reserve(this->Num() + static_cast<int32>(Values.size()));
            for (const value_type& Value : Values)
            {
                Add(Value);
            }
        }

        NODISCARD TVector<value_type> Array() const
        {
            TVector<value_type> Out;
            Out.Reserve(this->Num());
            for (const value_type& Value : *this)
            {
                Out.Add(Value);
            }
            return Out;
        }

        NODISCARD friend bool operator==(const TBasicHashSet& Left, const TBasicHashSet& Right) noexcept
        {
            if (Left.Num() != Right.Num())
            {
                return false;
            }
            for (const value_type& Element : Left)
            {
                if (!Right.Contains(Element))
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
        using KeyType     = key_type;
        using ValueType   = mapped_type;
        using ElementType = value_type;

        // Sets Key to Value, replacing what it held, as Unreal's TMap::Add does.
        template <typename TKeyArg, typename TValueArg>
        mapped_type& Add(TKeyArg&& Key, TValueArg&& Value)
        {
            return InsertOrAssign(std::forward<TKeyArg>(Key), std::forward<TValueArg>(Value)).Key->Value;
        }

        template <typename TKeyArg>
        mapped_type& Add(TKeyArg&& Key)
        {
            return InsertOrAssign(std::forward<TKeyArg>(Key), mapped_type()).Key->Value;
        }

        template <typename TKeyArg, typename... TArgs>
        mapped_type& Emplace(TKeyArg&& Key, TArgs&&... Args)
        {
            const TPair<iterator, bool> Placed = TryEmplace(std::forward<TKeyArg>(Key), std::forward<TArgs>(Args)...);
            if (!Placed.Value)
            {
                Placed.Key->Value = mapped_type(std::forward<TArgs>(Args)...);
            }
            return Placed.Key->Value;
        }

        // The value under Key, default-constructed and added first when there is none.
        template <typename TKeyArg>
        mapped_type& FindOrAdd(TKeyArg&& Key)
        {
            return TryEmplace(std::forward<TKeyArg>(Key)).Key->Value;
        }

        template <typename TKeyArg, typename TArg, typename... TArgs>
        mapped_type& FindOrAdd(TKeyArg&& Key, TArg&& Arg, TArgs&&... Args)
        {
            return TryEmplace(std::forward<TKeyArg>(Key), std::forward<TArg>(Arg), std::forward<TArgs>(Args)...).Key->Value;
        }

        // Adds Key unless it is present, leaving an existing value alone, and says which happened.
        template <typename TKeyArg, typename... TArgs>
        bool TryAdd(TKeyArg&& Key, TArgs&&... Args)
        {
            return TryEmplace(std::forward<TKeyArg>(Key), std::forward<TArgs>(Args)...).Value;
        }

        template <typename TKeyArg, typename TSelf>
        NODISCARD FORCEINLINE auto Find(this TSelf&& Self, const TKeyArg& Key) noexcept -> TLikeSelf<TSelf, mapped_type>*
        {
            auto* Pair = Self.FindElement(Key);
            return Pair != nullptr ? &Pair->Value : nullptr;
        }

        // A copy of the value under Key, or a default one when there is none.
        template <typename TKeyArg>
        NODISCARD mapped_type FindRef(const TKeyArg& Key) const
        {
            const mapped_type* Found = Find(Key);
            return Found != nullptr ? *Found : mapped_type();
        }

        template <typename TKeyArg, typename TSelf>
        NODISCARD auto FindChecked(this TSelf&& Self, const TKeyArg& Key) -> TLikeSelf<TSelf, mapped_type>&
        {
            auto* Found = Self.Find(Key);
            LUMINA_CONTAINER_CHECK(Found != nullptr);
            return *Found;
        }

        template <typename TKeyArg>
        bool RemoveAndCopyValue(const TKeyArg& Key, mapped_type& OutValue)
        {
            const iterator It = this->FindIt(Key);
            if (It == this->end())
            {
                return false;
            }
            OutValue = std::move(It->Value);
            this->EraseIterator(It);
            return true;
        }

        void GenerateKeyArray(TVector<key_type>& OutKeys) const
        {
            OutKeys.Reset(this->Num());
            for (const value_type& Pair : *this)
            {
                OutKeys.Add(Pair.Key);
            }
        }

        void GenerateValueArray(TVector<mapped_type>& OutValues) const
        {
            OutValues.Reset(this->Num());
            for (const value_type& Pair : *this)
            {
                OutValues.Add(Pair.Value);
            }
        }

        void Append(const TBasicHashMap& Other)
        {
            this->Reserve(this->Num() + Other.Num());
            for (const value_type& Pair : Other)
            {
                Add(Pair.Key, Pair.Value);
            }
        }

        // Find-or-add, as the standard map has it; Unreal's asserts, which would turn every insert written this way into a crash.
        NODISCARD mapped_type& operator[](const key_type& Key) { return FindOrAdd(Key); }
        NODISCARD mapped_type& operator[](key_type&& Key) { return FindOrAdd(std::move(Key)); }

        NODISCARD friend bool operator==(const TBasicHashMap& Left, const TBasicHashMap& Right) noexcept
        {
            if (Left.Num() != Right.Num())
            {
                return false;
            }
            for (const auto& Pair : Left)
            {
                const mapped_type* Found = Right.Find(Pair.Key);
                if (Found == nullptr || !(*Found == Pair.Value))
                {
                    return false;
                }
            }
            return true;
        }

    private:

        template <typename TKeyArg, typename... TArgs>
        TPair<iterator, bool> TryEmplace(TKeyArg&& Key, TArgs&&... Args)
        {
            if (this->MayRehashOnInsert())
            {
                return TryEmplaceStaged(std::forward<TKeyArg>(Key), std::forward<TArgs>(Args)...);
            }
            const TPair<size_t, bool> Placed = this->FindOrPrepareInsert(Key);
            if (Placed.Value)
            {
                TPolicy::Construct(this->SlotAt(Placed.Key),
                    std::piecewise_construct,
                    ForwardAsTuple(std::forward<TKeyArg>(Key)),
                    ForwardAsTuple(std::forward<TArgs>(Args)...));
            }
            return { this->IteratorAt(Placed.Key), Placed.Value };
        }

        template <typename TKeyArg, typename TValueArg>
        TPair<iterator, bool> InsertOrAssign(TKeyArg&& Key, TValueArg&& Value)
        {
            if (this->MayRehashOnInsert())
            {
                if (const iterator Existing = this->FindIt(Key); Existing != this->end())
                {
                    Existing->Value = std::forward<TValueArg>(Value);
                    return { Existing, false };
                }
                return TryEmplaceStaged(std::forward<TKeyArg>(Key), std::forward<TValueArg>(Value));
            }
            const TPair<size_t, bool> Placed = this->FindOrPrepareInsert(Key);
            if (Placed.Value)
            {
                TPolicy::Construct(this->SlotAt(Placed.Key),
                    std::piecewise_construct,
                    ForwardAsTuple(std::forward<TKeyArg>(Key)),
                    ForwardAsTuple(std::forward<TValueArg>(Value)));
            }
            else
            {
                TPolicy::Value(*this->SlotAt(Placed.Key)) = std::forward<TValueArg>(Value);
            }
            return { this->IteratorAt(Placed.Key), Placed.Value };
        }

        // The key and value are built before the table grows, so neither can be read from a slot the rehash moved.
        template <typename TKeyArg, typename... TArgs>
        TPair<iterator, bool> TryEmplaceStaged(TKeyArg&& Key, TArgs&&... Args)
        {
            if (const iterator Existing = this->FindIt(Key); Existing != this->end())
            {
                return { Existing, false };
            }

            key_type StagedKey(std::forward<TKeyArg>(Key));
            if constexpr (std::is_move_constructible_v<mapped_type>)
            {
                mapped_type StagedValue(std::forward<TArgs>(Args)...);
                const TPair<size_t, bool> Placed = this->FindOrPrepareInsert(StagedKey);
                TPolicy::Construct(this->SlotAt(Placed.Key), std::piecewise_construct,
                    ForwardAsTuple(std::move(StagedKey)), ForwardAsTuple(std::move(StagedValue)));
                return { this->IteratorAt(Placed.Key), true };
            }
            else
            {
                const TPair<size_t, bool> Placed = this->FindOrPrepareInsert(StagedKey);
                TPolicy::Construct(this->SlotAt(Placed.Key), std::piecewise_construct,
                    ForwardAsTuple(std::move(StagedKey)), ForwardAsTuple(std::forward<TArgs>(Args)...));
                return { this->IteratorAt(Placed.Key), true };
            }
        }
    };

    // Elements live in the table, so a rehash moves them and no reference into one survives an insert.
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

    // Elements are separately allocated, so an element address stays valid for as long as the element does.
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

    // Carries room for N entries inline; once it outgrows them it moves to the heap and stays there.
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

    template <typename K, typename V>
    using TScratchHashMap = TBasicHashMap<TMapPolicy<K, V>, FDefaultHash, FDefaultEqual, FScratchAllocator, 0>;

    template <typename T>
    using TScratchHashSet = TBasicHashSet<TSetPolicy<T>, FDefaultHash, FDefaultEqual, FScratchAllocator, 0>;
}

namespace Lumina
{
    // A heap-only table points only at its own heap block or the shared empty group, never into itself.
    template <typename TPolicy, typename THasher, typename TEqual, typename TAllocator>
    struct TIsTriviallyRelocatable<Containers::TBasicHashMap<TPolicy, THasher, TEqual, TAllocator, 0>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<THasher> && TIsTriviallyRelocatable_V<TEqual>;
    };

    template <typename TPolicy, typename THasher, typename TEqual, typename TAllocator>
    struct TIsTriviallyRelocatable<Containers::TBasicHashSet<TPolicy, THasher, TEqual, TAllocator, 0>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<THasher> && TIsTriviallyRelocatable_V<TEqual>;
    };

    template <typename K, typename V,
              typename H = Containers::FDefaultHash,
              typename E = Containers::FDefaultEqual>
    using THashMap = Containers::THashMap<K, V, H, E>;

    template <typename T,
              typename H = Containers::FDefaultHash,
              typename E = Containers::FDefaultEqual>
    using THashSet = Containers::THashSet<T, H, E>;

    template <typename K, typename V, size_t N, typename H = Containers::FDefaultHash, typename E = Containers::FDefaultEqual>
    using TInlineHashMap = Containers::TInlineHashMap<K, V, N, H, E>;

    template <typename T, size_t N, typename H = Containers::FDefaultHash, typename E = Containers::FDefaultEqual>
    using TInlineHashSet = Containers::TInlineHashSet<T, N, H, E>;
}
