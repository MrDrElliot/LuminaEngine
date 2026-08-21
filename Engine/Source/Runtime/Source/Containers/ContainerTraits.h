#pragma once

#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "Core/Assertions/CheckFailure.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#if defined(_MSC_VER)
    #define LUMINA_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
    #define LUMINA_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

#if !defined(LUMINA_CONTAINER_CHECKS)
    #if defined(LE_SHIPPING)
        #define LUMINA_CONTAINER_CHECKS 0
    #else
        #define LUMINA_CONTAINER_CHECKS 1
    #endif
#endif

// The out-of-line handler, not Assert.h itself: that reaches FString, and every container includes this.
// _INDEX and _WITHIN are the bounds forms; they report the two values, not just the expression text.
#if LUMINA_CONTAINER_CHECKS
    #define LUMINA_CONTAINER_CHECK(Expr) do { if (!(Expr)) [[unlikely]] { ::Lumina::Assert::Detail::HandleCheckFailure(#Expr, std::source_location::current()); } } while (false)
    #define LUMINA_CONTAINER_CHECK_BOUNDS(IndexExpr, Op, OpText, BoundExpr) do { const uint64 LuminaCheckedIndex = (uint64)(IndexExpr); const uint64 LuminaCheckedBound = (uint64)(BoundExpr); if (!(LuminaCheckedIndex Op LuminaCheckedBound)) [[unlikely]] { ::Lumina::Assert::Detail::HandleBoundsFailure(#IndexExpr, LuminaCheckedIndex, OpText, #BoundExpr, LuminaCheckedBound, std::source_location::current()); } } while (false)
    #define LUMINA_CONTAINER_CHECK_INDEX(IndexExpr, BoundExpr)  LUMINA_CONTAINER_CHECK_BOUNDS(IndexExpr, <, "<", BoundExpr)
    #define LUMINA_CONTAINER_CHECK_WITHIN(IndexExpr, BoundExpr) LUMINA_CONTAINER_CHECK_BOUNDS(IndexExpr, <=, "<=", BoundExpr)
#else
    #define LUMINA_CONTAINER_CHECK(Expr) ((void)0)
    #define LUMINA_CONTAINER_CHECK_BOUNDS(IndexExpr, Op, OpText, BoundExpr) ((void)0)
    #define LUMINA_CONTAINER_CHECK_INDEX(IndexExpr, BoundExpr) ((void)0)
    #define LUMINA_CONTAINER_CHECK_WITHIN(IndexExpr, BoundExpr) ((void)0)
#endif

namespace Lumina
{
    /** A type whose value is preserved by moving its bytes, with no move-construct or destroy needed. */
    template <typename T>
    struct TIsTriviallyRelocatable
    {
        static constexpr bool Value = std::is_trivially_copyable_v<T>;
    };

    template <typename T>
    inline constexpr bool TIsTriviallyRelocatable_V = TIsTriviallyRelocatable<T>::Value;

    template <typename T>
    concept TriviallyRelocatable = TIsTriviallyRelocatable_V<T>;

    template <typename A, typename B>
    struct TIsTriviallyRelocatable<std::pair<A, B>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<A> && TIsTriviallyRelocatable_V<B>;
    };

    template <typename... Ts>
    struct TIsTriviallyRelocatable<std::tuple<Ts...>>
    {
        static constexpr bool Value = (TIsTriviallyRelocatable_V<Ts> && ...);
    };

    template <typename T>
    struct TIsTriviallyRelocatable<std::optional<T>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<T>;
    };

    template <typename T, size_t N>
    struct TIsTriviallyRelocatable<T[N]>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<T>;
    };

    /** Value-initializing this type is equivalent to zeroing its bytes, so a range of them can be memset. */
    template <typename T>
    struct TIsZeroConstructible
    {
        static constexpr bool Value = std::is_scalar_v<T> || std::is_pointer_v<T>;
    };

    template <typename T>
    inline constexpr bool TIsZeroConstructible_V = TIsZeroConstructible<T>::Value;

    template <typename T>
    concept ZeroConstructible = TIsZeroConstructible_V<T>;

    template <typename T, size_t N>
    struct TIsZeroConstructible<T[N]>
    {
        static constexpr bool Value = TIsZeroConstructible_V<T>;
    };

    /** Comparing a range of these for equality is a memcmp; no per-element operator== call is needed. */
    template <typename T>
    struct TIsBitwiseComparable
    {
        static constexpr bool Value = std::is_scalar_v<T> && !std::is_floating_point_v<T>;
    };

    template <typename T>
    inline constexpr bool TIsBitwiseComparable_V = TIsBitwiseComparable<T>::Value;
}

/** Opts a type into byte-wise relocation, at global scope. It must hold no pointer or reference into itself. */
#define LUMINA_DECLARE_TRIVIALLY_RELOCATABLE(...)                       \
    namespace Lumina                                                    \
    {                                                                   \
        template <>                                                     \
        struct TIsTriviallyRelocatable<__VA_ARGS__>                     \
        {                                                               \
            static constexpr bool Value = true;                         \
        };                                                              \
    }

/** Opts a type into memset-based value initialization, at global scope. */
#define LUMINA_DECLARE_ZERO_CONSTRUCTIBLE(...)                          \
    namespace Lumina                                                    \
    {                                                                   \
        template <>                                                     \
        struct TIsZeroConstructible<__VA_ARGS__>                        \
        {                                                               \
            static constexpr bool Value = true;                         \
        };                                                              \
    }
