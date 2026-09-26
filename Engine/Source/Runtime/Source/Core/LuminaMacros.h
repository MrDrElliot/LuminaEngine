#pragma once

#include "Platform/GenericPlatform.h"

#include <utility>

#define BIT(x) (1 << (x))
#define BIT64(x) (1ULL << (x))

#define CAT(x, y) CAT_(x, y)
#define CAT_(x, y) x##y

#define LE_EPSILON                      1.192092896e-07F
#define LE_SMALL_NUMBER                 1e-8f
#define LE_KINDA_SMALL_NUMBER           1e-4f
#define LE_KINDA_SORTA_SMALL_NUMBER     1e-2f

#define LE_BIG_NUMBER          3.4e+38f
#define LE_MAX_FLOAT           3.402823466e+38f
#define LE_MIN_FLOAT           1.175494351e-38f

#define LE_PI                  3.14159265358979323846
#define LE_PI_F                3.14159265358979323846f
#define LE_TWO_PI              (2.0 * LE_PI)
#define LE_HALF_PI             (0.5 * LE_PI)

#define LE_DEG2RAD(x)          ((x) * (LE_PI / 180.0))
#define LE_RAD2DEG(x)          ((x) * (180.0 / LE_PI))

#define USING(flag) DETAIL_USING_FIRST(CAT(DETAIL_USING_CHECK_, flag))
#define DETAIL_USING_CHECK_0 0,x
#define DETAIL_USING_CHECK_1 1,x
#if defined(_MSC_VER) && !defined(__clang__) && (!defined(_MSVC_TRADITIONAL) || _MSVC_TRADITIONAL) // Legacy MSVC preprocessor workaround.
#define DETAIL_USING_DUMMY
#define DETAIL_USING_FIRST(...) DETAIL_USING_FIRST_ DETAIL_USING_DUMMY(__VA_ARGS__)
#else
#define DETAIL_USING_FIRST(...) DETAIL_USING_FIRST_(__VA_ARGS__)
#endif
#define DETAIL_USING_FIRST_(x, y) x

#define STRINGIFY_DETAIL(x) #x
#define STRINGIFY(x) STRINGIFY_DETAIL(x)

#define LUM_DEPRECATED(Version, Reason) [[deprecated("Deprecated since " #Version ": " Reason)]]

#define LE_NO_COPY(X) \
    X(const X&) = delete; \
    X& operator = (const X&) = delete \

#define LE_NO_MOVE(X) \
    X(X&&) = delete; \
    X& operator = (X&&) = delete \

#define LE_NO_COPYMOVE(X) \
    LE_NO_COPY(X); \
    LE_NO_MOVE(X) \
    
#define LE_DEFAULT_MOVE(X) \
    X(X&&) = default; \
    X& operator = (X&&) = default \

#define ENUM_CLASS_FLAGS(Enum) \
inline           Enum& operator|=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)(std::to_underlying(Lhs) | std::to_underlying(Rhs)); } \
inline           Enum& operator&=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)(std::to_underlying(Lhs) & std::to_underlying(Rhs)); } \
inline           Enum& operator^=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)(std::to_underlying(Lhs) ^ std::to_underlying(Rhs)); } \
inline constexpr Enum  operator| (Enum  Lhs, Enum Rhs) { return (Enum)(std::to_underlying(Lhs) | std::to_underlying(Rhs)); } \
inline constexpr Enum  operator& (Enum  Lhs, Enum Rhs) { return (Enum)(std::to_underlying(Lhs) & std::to_underlying(Rhs)); } \
inline constexpr Enum  operator^ (Enum  Lhs, Enum Rhs) { return (Enum)(std::to_underlying(Lhs) ^ std::to_underlying(Rhs)); } \
inline constexpr bool  operator! (Enum  E)             { return !std::to_underlying(E); } \
inline constexpr Enum  operator~ (Enum  E)             { return (Enum)~std::to_underlying(E); } \
static_assert(true, "ENUM_CLASS_FLAGS consumes the semicolon at the call site")

template<typename Enum>
[[nodiscard]] constexpr bool EnumHasAllFlags(Enum Flags, Enum Contains)
{
    return (std::to_underlying(Flags) & std::to_underlying(Contains)) == std::to_underlying(Contains);
}

template<typename Enum>
[[nodiscard]] constexpr bool EnumHasAnyFlags(Enum Flags, Enum Contains)
{
    return (std::to_underlying(Flags) & std::to_underlying(Contains)) != 0;
}

template<typename Enum>
constexpr void EnumAddFlags(Enum& Flags, Enum FlagsToAdd)
{
    Flags = (Enum)(std::to_underlying(Flags) | std::to_underlying(FlagsToAdd));
}

template<typename Enum>
constexpr void EnumRemoveFlags(Enum& Flags, Enum FlagsToRemove)
{
    Flags = (Enum)(std::to_underlying(Flags) & ~std::to_underlying(FlagsToRemove));
}
