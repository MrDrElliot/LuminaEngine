#pragma once

#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#if defined(_MSC_VER)
    #define LUMINA_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
    #define LUMINA_CONTAINER_TRAP() __debugbreak()
#else
    #define LUMINA_NO_UNIQUE_ADDRESS [[no_unique_address]]
    #define LUMINA_CONTAINER_TRAP() __builtin_trap()
#endif

#if !defined(LUMINA_CONTAINER_CHECKS)
    #if defined(LE_SHIPPING)
        #define LUMINA_CONTAINER_CHECKS 0
    #else
        #define LUMINA_CONTAINER_CHECKS 1
    #endif
#endif

// Deliberately not Assert.h; that reaches FString, <format> and <stacktrace>, and every container includes this.
#if LUMINA_CONTAINER_CHECKS
    #define LUMINA_CONTAINER_CHECK(Expr) do { if (!(Expr)) { LUMINA_CONTAINER_TRAP(); } } while (false)
#else
    #define LUMINA_CONTAINER_CHECK(Expr) ((void)0)
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

    // The standard smart pointers are a pointer pair with no self-reference on any toolchain we build for.
    template <typename T, typename D>
    struct TIsTriviallyRelocatable<std::unique_ptr<T, D>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<D>;
    };

    template <typename T>
    struct TIsTriviallyRelocatable<std::shared_ptr<T>>
    {
        static constexpr bool Value = true;
    };

    template <typename T>
    struct TIsTriviallyRelocatable<std::weak_ptr<T>>
    {
        static constexpr bool Value = true;
    };

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
