#pragma once

#include <array>
#include <initializer_list>
#include <iterator>
#include <span>
#include <type_traits>

#include "ContainerTraits.h"
#include "Core/Templates/NumericLimits.h"

namespace Lumina::Containers
{
    template <typename T, typename TSizeType = int32>
    class TSpan;

    namespace Private
    {
        template <typename T>
        inline constexpr bool bIsSpan = false;

        template <typename T, typename TSizeType>
        inline constexpr bool bIsSpan<TSpan<T, TSizeType>> = true;

        template <typename TRange>
        concept NotSpanOrArray = !bIsSpan<std::remove_cvref_t<TRange>> && !std::is_array_v<std::remove_cvref_t<TRange>>;

        template <typename TRange, typename TElement>
        concept OwnContiguousRange = NotSpanOrArray<TRange> && requires(TRange& Range)
        {
            { Range.GetData() } -> std::convertible_to<TElement*>;
            { Range.Num() } -> std::convertible_to<int64>;
        };

        template <typename TRange, typename TElement>
        concept StdContiguousRange = NotSpanOrArray<TRange> && !OwnContiguousRange<TRange, TElement> && requires(TRange& Range)
        {
            { Range.data() } -> std::convertible_to<TElement*>;
            { Range.size() } -> std::convertible_to<size_t>;
        };

        template <typename TRange, bool bOwn = requires(TRange& Range) { Range.GetData(); }>
        struct TRangeElement
        {
            using Type = std::remove_pointer_t<decltype(std::declval<TRange&>().GetData())>;
        };

        template <typename TRange>
        struct TRangeElement<TRange, false>
        {
            using Type = std::remove_pointer_t<decltype(std::declval<TRange&>().data())>;
        };

        template <typename TRange>
        struct TRangeSizeType
        {
            using Type = int32;
        };

        template <typename TRange>
        requires requires(TRange& Range) { { Range.Num() } -> std::same_as<int64>; }
        struct TRangeSizeType<TRange>
        {
            using Type = int64;
        };

        // A count read into a narrower span is checked, while one that already fits passes as is.
        template <typename TSizeType, typename TCount>
        NODISCARD FORCEINLINE constexpr TSizeType FitSpanCount(TCount InCount) noexcept
        {
            if constexpr (sizeof(TCount) > sizeof(TSizeType) || (std::is_unsigned_v<TCount> && sizeof(TCount) == sizeof(TSizeType)))
            {
                return static_cast<TSizeType>(CheckCapacity(static_cast<uint64>(InCount), static_cast<uint64>(TNumericLimits<TSizeType>::Max())));
            }
            else
            {
                return static_cast<TSizeType>(InCount);
            }
        }

        // Only a 64-bit counted container of ours narrows explicitly; an unsigned legacy count stays implicit but checked.
        template <typename TRange, typename TSizeType>
        inline constexpr bool bRangeCountIsWider = std::is_signed_v<decltype(std::declval<TRange&>().Num())>
            && sizeof(decltype(std::declval<TRange&>().Num())) > sizeof(TSizeType);
    }

    /** Borrowed contiguous range: a pointer and a count, always dynamically sized; TSpan64 counts in int64. */
    template <typename T, typename TSizeType>
    class TSpan
    {
        static_assert(std::is_same_v<TSizeType, int32> || std::is_same_v<TSizeType, int64>, "A span counts in int32 or int64");

    public:

        using SizeType               = TSizeType;
        using USizeType              = std::make_unsigned_t<TSizeType>;

        using element_type           = T;
        using value_type             = std::remove_cv_t<T>;
        using size_type              = SizeType;
        using difference_type        = ptrdiff_t;
        using pointer                = T*;
        using const_pointer          = const T*;
        using reference              = T&;
        using const_reference        = const T&;
        using iterator               = T*;
        using const_iterator         = const T*;
        using reverse_iterator       = std::reverse_iterator<T*>;
        using const_reverse_iterator = std::reverse_iterator<const T*>;

        constexpr TSpan() noexcept = default;

        constexpr TSpan(T* InData, size_type InSize) noexcept
            : Elements(InData)
            , Count(InSize)
        {
            LUMINA_CONTAINER_CHECK(InSize >= 0);
        }

        constexpr TSpan(T* First, T* Last) noexcept
            : Elements(First)
            , Count(static_cast<SizeType>(Private::CheckCapacity(static_cast<uint64>(Last - First), static_cast<uint64>(TNumericLimits<SizeType>::Max()))))
        {
        }

        template <size_t N>
        constexpr TSpan(T (&Array)[N]) noexcept
            : Elements(Array)
            , Count(static_cast<SizeType>(N))
        {}

        template <typename TRange>
        requires Private::OwnContiguousRange<TRange, T>
        constexpr explicit(Private::bRangeCountIsWider<TRange, SizeType>) TSpan(TRange&& Range) noexcept
            : Elements(Range.GetData())
            , Count(Private::FitSpanCount<SizeType>(Range.Num()))
        {}

        template <typename TRange>
        requires Private::StdContiguousRange<TRange, T>
        constexpr TSpan(TRange&& Range) noexcept
            : Elements(Range.data())
            , Count(Private::FitSpanCount<SizeType>(Range.size()))
        {}

        constexpr TSpan(std::initializer_list<value_type> Values) noexcept
            requires std::is_const_v<T>
            : Elements(Values.begin())
            , Count(Private::FitSpanCount<SizeType>(Values.size()))
        {}

        constexpr TSpan(std::span<T> Other) noexcept
            : Elements(Other.data())
            , Count(Private::FitSpanCount<SizeType>(Other.size()))
        {}

        template <typename TOther, typename TOtherSize>
        requires (!std::is_same_v<TOtherSize, TSizeType> && std::is_convertible_v<TOther (*)[], T (*)[]>)
        constexpr explicit(sizeof(TOtherSize) > sizeof(TSizeType)) TSpan(TSpan<TOther, TOtherSize> Other) noexcept
            : Elements(Other.GetData())
            , Count(Private::FitSpanCount<SizeType>(Other.Num()))
        {}

        NODISCARD constexpr operator std::span<T>() const noexcept { return std::span<T>(Elements, USize()); }

        NODISCARD constexpr operator TSpan<const T, TSizeType>() const noexcept
            requires (!std::is_const_v<T>)
        {
            return TSpan<const T, TSizeType>(Elements, Num());
        }

        NODISCARD FORCEINLINE constexpr T* GetData() const noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr SizeType Num() const noexcept { return Count; }
        NODISCARD FORCEINLINE constexpr size_t NumBytes() const noexcept { return static_cast<size_t>(USize()) * sizeof(T); }
        NODISCARD FORCEINLINE constexpr bool IsEmpty() const noexcept { return Count == 0; }
        NODISCARD FORCEINLINE constexpr bool IsValidIndex(SizeType Index) const noexcept { return static_cast<USizeType>(Index) < USize(); }

        NODISCARD FORCEINLINE constexpr T* begin() const noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr T* end() const noexcept { return Elements + USize(); }
        NODISCARD FORCEINLINE constexpr const T* cbegin() const noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr const T* cend() const noexcept { return Elements + USize(); }

        NODISCARD constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        NODISCARD constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

        NODISCARD FORCEINLINE constexpr T& operator[](size_type Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, USize());
            return Elements[Index];
        }

        NODISCARD FORCEINLINE constexpr T& Last(SizeType IndexFromEnd = 0) const noexcept { return (*this)[Num() - IndexFromEnd - 1]; }

        // Clamped like Unreal's, so a start or count past either end yields what overlaps rather than asserting.
        NODISCARD constexpr TSpan Mid(SizeType Start, SizeType InCount = TNumericLimits<SizeType>::Max()) const noexcept
        {
            const USizeType First = Start < 0 ? 0u : (static_cast<USizeType>(Start) < USize() ? static_cast<USizeType>(Start) : USize());
            const USizeType Wanted = InCount < 0 ? 0u : static_cast<USizeType>(InCount);
            const USizeType Available = USize() - First;
            return FromRaw(Elements + First, Wanted < Available ? Wanted : Available);
        }

        NODISCARD FORCEINLINE constexpr TSpan Left(SizeType InCount) const noexcept { return Mid(0, InCount); }
        NODISCARD FORCEINLINE constexpr TSpan LeftChop(SizeType InCount) const noexcept { return Mid(0, Num() - (InCount < 0 ? 0 : InCount)); }
        NODISCARD FORCEINLINE constexpr TSpan RightChop(SizeType InCount) const noexcept { return Mid(InCount < 0 ? 0 : InCount); }
        NODISCARD FORCEINLINE constexpr TSpan Right(SizeType InCount) const noexcept
        {
            const SizeType Taken = InCount < 0 ? 0 : (InCount < Num() ? InCount : Num());
            return Mid(Num() - Taken, Taken);
        }

        template <typename TKey>
        NODISCARD constexpr SizeType Find(const TKey& Value) const noexcept
        {
            for (USizeType Index = 0; Index < USize(); ++Index)
            {
                if (Elements[Index] == Value)
                {
                    return static_cast<SizeType>(Index);
                }
            }
            return Constants::kIndexNone;
        }

        template <typename TKey>
        NODISCARD constexpr bool Contains(const TKey& Value) const noexcept { return Find(Value) != Constants::kIndexNone; }

    private:

        NODISCARD static constexpr TSpan FromRaw(T* InData, USizeType InCount) noexcept
        {
            TSpan Result;
            Result.Elements = InData;
            Result.Count = static_cast<SizeType>(InCount);
            return Result;
        }

        NODISCARD FORCEINLINE constexpr USizeType USize() const noexcept { return static_cast<USizeType>(Count); }

        T*    Elements = nullptr;
        SizeType Count    = 0;
    };

    template <typename T, std::integral TSize>
    TSpan(T*, TSize) -> TSpan<T>;

    template <typename T, size_t N>
    TSpan(T (&)[N]) -> TSpan<T>;

    template <typename TRange>
    TSpan(TRange&&) -> TSpan<typename Private::TRangeElement<TRange>::Type, typename Private::TRangeSizeType<std::remove_cvref_t<TRange>>::Type>;

    template <typename T, typename TSizeType>
    NODISCARD constexpr TSpan<const uint8, TSizeType> AsBytes(TSpan<T, TSizeType> Span) noexcept
    {
        return TSpan<const uint8, TSizeType>(reinterpret_cast<const uint8*>(Span.GetData()), Private::FitSpanCount<TSizeType>(Span.NumBytes()));
    }

    template <typename T, typename TSizeType>
    requires (!std::is_const_v<T>)
    NODISCARD constexpr TSpan<uint8, TSizeType> AsWritableBytes(TSpan<T, TSizeType> Span) noexcept
    {
        return TSpan<uint8, TSizeType>(reinterpret_cast<uint8*>(Span.GetData()), Private::FitSpanCount<TSizeType>(Span.NumBytes()));
    }
}

namespace Lumina
{
    template <typename T, typename TSizeType = int32>
    using TSpan = Containers::TSpan<T, TSizeType>;

    template <typename T>
    using TSpan64 = Containers::TSpan<T, int64>;
}

// A span borrows, so a ranges algorithm on a temporary one returns a usable iterator rather than a dangling marker.
namespace std::ranges
{
    template <typename T, typename TSizeType>
    inline constexpr bool enable_borrowed_range<Lumina::Containers::TSpan<T, TSizeType>> = true;
}
