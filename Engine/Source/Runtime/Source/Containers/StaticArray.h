#pragma once

#include <compare>
#include <iterator>
#include <type_traits>
#include <utility>

#include "ContainerTraits.h"

namespace Lumina::Containers
{
    /** Fixed-size array with the size in the type. An aggregate, so brace initialization still works. */
    template <typename T, size_t N>
    struct TArray
    {
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

        T Elements[N];

        NODISCARD FORCEINLINE constexpr T* data() noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr const T* data() const noexcept { return Elements; }

        NODISCARD static constexpr size_t size() noexcept { return N; }
        NODISCARD static constexpr size_t max_size() noexcept { return N; }
        NODISCARD static constexpr bool empty() noexcept { return N == 0; }
        NODISCARD static constexpr size_t Num() noexcept { return N; }

        NODISCARD FORCEINLINE constexpr T* begin() noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr const T* begin() const noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr const T* cbegin() const noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr T* end() noexcept { return Elements + N; }
        NODISCARD FORCEINLINE constexpr const T* end() const noexcept { return Elements + N; }
        NODISCARD FORCEINLINE constexpr const T* cend() const noexcept { return Elements + N; }

        NODISCARD constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        NODISCARD constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        NODISCARD constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        NODISCARD constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

        NODISCARD FORCEINLINE constexpr T& operator[](size_t Index) noexcept
        {
            LUMINA_CONTAINER_CHECK(Index < N);
            return Elements[Index];
        }

        NODISCARD FORCEINLINE constexpr const T& operator[](size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Index < N);
            return Elements[Index];
        }

        NODISCARD FORCEINLINE constexpr T& at(size_t Index) noexcept { return (*this)[Index]; }
        NODISCARD FORCEINLINE constexpr const T& at(size_t Index) const noexcept { return (*this)[Index]; }

        NODISCARD FORCEINLINE constexpr T& front() noexcept { return (*this)[0]; }
        NODISCARD FORCEINLINE constexpr const T& front() const noexcept { return (*this)[0]; }
        NODISCARD FORCEINLINE constexpr T& back() noexcept { return (*this)[N - 1]; }
        NODISCARD FORCEINLINE constexpr const T& back() const noexcept { return (*this)[N - 1]; }

        constexpr void fill(const T& Value)
        {
            for (size_t Index = 0; Index < N; ++Index)
            {
                Elements[Index] = Value;
            }
        }

        constexpr void swap(TArray& Other) noexcept(std::is_nothrow_swappable_v<T>)
        {
            using std::swap;
            for (size_t Index = 0; Index < N; ++Index)
            {
                swap(Elements[Index], Other.Elements[Index]);
            }
        }

        NODISCARD friend constexpr bool operator==(const TArray& Left, const TArray& Right)
        {
            for (size_t Index = 0; Index < N; ++Index)
            {
                if (!(Left.Elements[Index] == Right.Elements[Index]))
                {
                    return false;
                }
            }
            return true;
        }

        NODISCARD friend constexpr auto operator<=>(const TArray& Left, const TArray& Right)
        {
            for (size_t Index = 0; Index < N; ++Index)
            {
                if (const auto Order = Left.Elements[Index] <=> Right.Elements[Index]; Order != 0)
                {
                    return Order;
                }
            }
            return decltype(Left.Elements[0] <=> Right.Elements[0])::equivalent;
        }
    };

    /** A zero-length array still has to be a valid type; it just has nothing to point at. */
    template <typename T>
    struct TArray<T, 0>
    {
        using value_type      = T;
        using size_type       = size_t;
        using reference       = T&;
        using const_reference = const T&;
        using pointer         = T*;
        using const_pointer   = const T*;
        using iterator        = T*;
        using const_iterator  = const T*;

        NODISCARD FORCEINLINE constexpr T* data() noexcept { return nullptr; }
        NODISCARD FORCEINLINE constexpr const T* data() const noexcept { return nullptr; }

        NODISCARD static constexpr size_t size() noexcept { return 0; }
        NODISCARD static constexpr size_t max_size() noexcept { return 0; }
        NODISCARD static constexpr bool empty() noexcept { return true; }
        NODISCARD static constexpr size_t Num() noexcept { return 0; }

        NODISCARD FORCEINLINE constexpr T* begin() noexcept { return nullptr; }
        NODISCARD FORCEINLINE constexpr const T* begin() const noexcept { return nullptr; }
        NODISCARD FORCEINLINE constexpr T* end() noexcept { return nullptr; }
        NODISCARD FORCEINLINE constexpr const T* end() const noexcept { return nullptr; }

        constexpr void fill(const T&) {}
        constexpr void swap(TArray&) noexcept {}

        NODISCARD friend constexpr bool operator==(const TArray&, const TArray&) { return true; }
    };

    template <typename T, size_t N>
    FORCEINLINE constexpr void swap(TArray<T, N>& Left, TArray<T, N>& Right) noexcept(noexcept(Left.swap(Right)))
    {
        Left.swap(Right);
    }

    template <size_t Index, typename T, size_t N>
    NODISCARD constexpr T& get(TArray<T, N>& Value) noexcept
    {
        static_assert(Index < N, "Element index is past the end of the array.");
        return Value.Elements[Index];
    }

    template <size_t Index, typename T, size_t N>
    NODISCARD constexpr const T& get(const TArray<T, N>& Value) noexcept
    {
        static_assert(Index < N, "Element index is past the end of the array.");
        return Value.Elements[Index];
    }
}

namespace Lumina
{
    template <typename T, size_t N>
    using TArray = Containers::TArray<T, N>;
}

namespace std
{
    template <typename T, size_t N>
    struct tuple_size<Lumina::Containers::TArray<T, N>> : integral_constant<size_t, N> {};

    template <size_t Index, typename T, size_t N>
    struct tuple_element<Index, Lumina::Containers::TArray<T, N>> { using type = T; };
}
