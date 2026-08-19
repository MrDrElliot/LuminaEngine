#pragma once

#include <array>
#include <initializer_list>
#include <iterator>
#include <span>
#include <type_traits>

#include "ContainerTraits.h"

namespace Lumina::Containers
{
    template <typename T>
    class TSpan;

    namespace Private
    {
        template <typename T>
        inline constexpr bool bIsSpan = false;

        template <typename T>
        inline constexpr bool bIsSpan<TSpan<T>> = true;

        template <typename T>
        inline constexpr bool bIsStdArray = false;

        template <typename T, size_t N>
        inline constexpr bool bIsStdArray<std::array<T, N>> = true;

        /** Anything contiguous whose element type converts to the span's, and that is not a span itself. */
        template <typename TRange, typename TElement>
        concept SpanCompatibleRange =
            !bIsSpan<std::remove_cvref_t<TRange>> &&
            !std::is_array_v<std::remove_cvref_t<TRange>> &&
            requires(TRange& Range)
            {
                { Range.data() } -> std::convertible_to<TElement*>;
                { Range.size() } -> std::convertible_to<size_t>;
            };
    }

    /** Borrowed contiguous range: a pointer and a count, always dynamically sized. */
    template <typename T>
    class TSpan
    {
    public:

        using element_type           = T;
        using value_type             = std::remove_cv_t<T>;
        using size_type              = size_t;
        using difference_type        = ptrdiff_t;
        using pointer                = T*;
        using const_pointer          = const T*;
        using reference              = T&;
        using const_reference        = const T&;
        using iterator               = T*;
        using const_iterator         = const T*;
        using reverse_iterator       = std::reverse_iterator<T*>;
        using const_reverse_iterator = std::reverse_iterator<const T*>;

        static constexpr size_t npos = ~static_cast<size_t>(0);

        constexpr TSpan() noexcept = default;

        constexpr TSpan(T* InData, size_t InSize) noexcept
            : Data(InData)
            , Size(InSize)
        {}

        constexpr TSpan(T* First, T* Last) noexcept
            : Data(First)
            , Size(static_cast<size_t>(Last - First))
        {}

        template <size_t N>
        constexpr TSpan(T (&Array)[N]) noexcept
            : Data(Array)
            , Size(N)
        {}

        template <typename TRange>
        requires Private::SpanCompatibleRange<TRange, T>
        constexpr TSpan(TRange&& Range) noexcept
            : Data(Range.data())
            , Size(Range.size())
        {}

        constexpr TSpan(std::initializer_list<value_type> Values) noexcept
            requires std::is_const_v<T>
            : Data(Values.begin())
            , Size(Values.size())
        {}

        constexpr TSpan(std::span<T> Other) noexcept
            : Data(Other.data())
            , Size(Other.size())
        {}

        NODISCARD constexpr operator std::span<T>() const noexcept { return std::span<T>(Data, Size); }

        NODISCARD constexpr operator TSpan<const T>() const noexcept
            requires (!std::is_const_v<T>)
        {
            return TSpan<const T>(Data, Size);
        }

        NODISCARD FORCEINLINE constexpr T* data() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr size_t size() const noexcept { return Size; }
        NODISCARD FORCEINLINE constexpr size_t size_bytes() const noexcept { return Size * sizeof(T); }
        NODISCARD FORCEINLINE constexpr bool empty() const noexcept { return Size == 0; }

        NODISCARD FORCEINLINE constexpr T* begin() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr T* end() const noexcept { return Data + Size; }
        NODISCARD FORCEINLINE constexpr const T* cbegin() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr const T* cend() const noexcept { return Data + Size; }

        NODISCARD constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        NODISCARD constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

        NODISCARD FORCEINLINE constexpr T& operator[](size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Index < Size);
            return Data[Index];
        }

        NODISCARD FORCEINLINE constexpr T& at(size_t Index) const noexcept { return (*this)[Index]; }

        NODISCARD FORCEINLINE constexpr T& front() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Size != 0);
            return Data[0];
        }

        NODISCARD FORCEINLINE constexpr T& back() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Size != 0);
            return Data[Size - 1];
        }

        NODISCARD constexpr TSpan first(size_t Count) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Count <= Size);
            return TSpan(Data, Count);
        }

        NODISCARD constexpr TSpan last(size_t Count) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Count <= Size);
            return TSpan(Data + (Size - Count), Count);
        }

        NODISCARD constexpr TSpan subspan(size_t Offset, size_t Count = npos) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Offset <= Size);
            const size_t Available = Size - Offset;
            return TSpan(Data + Offset, Count < Available ? Count : Available);
        }

        NODISCARD FORCEINLINE constexpr TSpan Left(size_t Count) const noexcept { return first(Count); }
        NODISCARD FORCEINLINE constexpr TSpan Right(size_t Count) const noexcept { return last(Count); }

        NODISCARD constexpr bool Contains(const T& Value) const noexcept
        {
            for (size_t Index = 0; Index < Size; ++Index)
            {
                if (Data[Index] == Value)
                {
                    return true;
                }
            }
            return false;
        }

        NODISCARD constexpr size_t IndexOf(const T& Value) const noexcept
        {
            for (size_t Index = 0; Index < Size; ++Index)
            {
                if (Data[Index] == Value)
                {
                    return Index;
                }
            }
            return npos;
        }

    private:

        T*     Data = nullptr;
        size_t Size = 0;
    };

    template <typename T>
    TSpan(T*, size_t) -> TSpan<T>;

    template <typename T, size_t N>
    TSpan(T (&)[N]) -> TSpan<T>;

    template <typename TRange>
    TSpan(TRange&&) -> TSpan<std::remove_pointer_t<decltype(std::declval<TRange&>().data())>>;

    template <typename T>
    NODISCARD constexpr TSpan<const uint8> AsBytes(TSpan<T> Span) noexcept
    {
        return TSpan<const uint8>(reinterpret_cast<const uint8*>(Span.data()), Span.size_bytes());
    }

    template <typename T>
    requires (!std::is_const_v<T>)
    NODISCARD constexpr TSpan<uint8> AsWritableBytes(TSpan<T> Span) noexcept
    {
        return TSpan<uint8>(reinterpret_cast<uint8*>(Span.data()), Span.size_bytes());
    }
}

namespace Lumina
{
    template <typename T>
    using TSpan = Containers::TSpan<T>;
}
