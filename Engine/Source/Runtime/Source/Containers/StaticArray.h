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
        using size_type              = int32;
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

        // Elements is an array member, so a const object deduces const T* and const T& on its own.
        template <typename TSelf> NODISCARD FORCEINLINE constexpr auto* GetData(this TSelf&& Self) noexcept { return Self.Elements; }

        NODISCARD static constexpr int32 Num() noexcept { return static_cast<int32>(N); }
        NODISCARD static constexpr bool IsEmpty() noexcept { return N == 0; }
        NODISCARD static constexpr bool IsValidIndex(int32 Index) noexcept { return static_cast<uint32>(Index) < N; }

        template <typename TSelf> NODISCARD FORCEINLINE constexpr auto* begin(this TSelf&& Self) noexcept { return Self.Elements; }
        template <typename TSelf> NODISCARD FORCEINLINE constexpr auto* end(this TSelf&& Self) noexcept { return Self.Elements + N; }
        NODISCARD FORCEINLINE constexpr const T* cbegin() const noexcept { return Elements; }
        NODISCARD FORCEINLINE constexpr const T* cend() const noexcept { return Elements + N; }

        template <typename TSelf> NODISCARD constexpr auto rbegin(this TSelf&& Self) noexcept { return std::reverse_iterator(Self.end()); }
        template <typename TSelf> NODISCARD constexpr auto rend(this TSelf&& Self) noexcept { return std::reverse_iterator(Self.begin()); }

        template <typename TSelf>
        NODISCARD FORCEINLINE constexpr auto& operator[](this TSelf&& Self, int32 Index) noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, N);
            return Self.Elements[Index];
        }

        template <typename TSelf> NODISCARD FORCEINLINE constexpr auto& Last(this TSelf&& Self, int32 IndexFromEnd = 0) noexcept { return Self[static_cast<int32>(N) - IndexFromEnd - 1]; }

        constexpr void Fill(const T& Value)
        {
            for (uint32 Index = 0; Index < N; ++Index)
            {
                Elements[Index] = Value;
            }
        }

        friend constexpr void swap(TArray& Left, TArray& Right) noexcept(std::is_nothrow_swappable_v<T>)
        {
            using std::swap;
            for (uint32 Index = 0; Index < N; ++Index)
            {
                swap(Left.Elements[Index], Right.Elements[Index]);
            }
        }

        NODISCARD friend constexpr bool operator==(const TArray& Left, const TArray& Right)
        {
            for (uint32 Index = 0; Index < N; ++Index)
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
            for (uint32 Index = 0; Index < N; ++Index)
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
        using size_type       = int32;
        using reference       = T&;
        using const_reference = const T&;
        using pointer         = T*;
        using const_pointer   = const T*;
        using iterator        = T*;
        using const_iterator  = const T*;

        template <typename TSelf> NODISCARD FORCEINLINE constexpr auto GetData(this TSelf&&) noexcept -> TLikeSelf<TSelf, T>* { return nullptr; }

        NODISCARD static constexpr int32 Num() noexcept { return 0; }
        NODISCARD static constexpr bool IsEmpty() noexcept { return true; }

        template <typename TSelf> NODISCARD FORCEINLINE constexpr auto begin(this TSelf&&) noexcept -> TLikeSelf<TSelf, T>* { return nullptr; }
        template <typename TSelf> NODISCARD FORCEINLINE constexpr auto end(this TSelf&&) noexcept -> TLikeSelf<TSelf, T>* { return nullptr; }

        constexpr void Fill(const T&) {}
        friend constexpr void swap(TArray&, TArray&) noexcept {}

        NODISCARD friend constexpr bool operator==(const TArray&, const TArray&) { return true; }
    };

    namespace Private
    {
        template <typename T>
        inline constexpr size_t kStaticArrayNum = ~static_cast<size_t>(0);

        template <typename T, size_t N>
        inline constexpr size_t kStaticArrayNum<TArray<T, N>> = N;
    }

    // One overload for every value category, so a structured binding by value moves out of its hidden copy.
    template <size_t Index, typename TSelf>
    requires (Private::kStaticArrayNum<std::remove_cvref_t<TSelf>> != ~static_cast<size_t>(0))
    NODISCARD constexpr auto&& get(TSelf&& Value) noexcept
    {
        static_assert(Index < Private::kStaticArrayNum<std::remove_cvref_t<TSelf>>, "Element index is past the end of the array.");
        return std::forward_like<TSelf>(Value.Elements[Index]);
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
