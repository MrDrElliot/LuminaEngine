#pragma once

#include "Platform/GenericPlatform.h"

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
inline           Enum& operator|=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
inline           Enum& operator&=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
inline           Enum& operator^=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
inline constexpr Enum  operator| (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
inline constexpr Enum  operator& (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
inline constexpr Enum  operator^ (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
inline constexpr bool  operator! (Enum  E)             { return !(__underlying_type(Enum))E; } \
inline constexpr Enum  operator~ (Enum  E)             { return (Enum)~(__underlying_type(Enum))E; }

template<typename Enum>
[[nodiscard]] constexpr bool EnumHasAllFlags(Enum Flags, Enum Contains)
{
    using UnderlyingType = __underlying_type(Enum);
    return ((UnderlyingType)Flags & (UnderlyingType)Contains) == (UnderlyingType)Contains;
}

template<typename Enum>
[[nodiscard]] constexpr bool EnumHasAnyFlags(Enum Flags, Enum Contains)
{
    using UnderlyingType = __underlying_type(Enum);
    return ((UnderlyingType)Flags & (UnderlyingType)Contains) != 0;
}

template<typename Enum>
void EnumAddFlags(Enum& Flags, Enum FlagsToAdd)
{
    using UnderlyingType = __underlying_type(Enum);
    Flags = (Enum)((UnderlyingType)Flags | (UnderlyingType)FlagsToAdd);
}

template<typename Enum>
void EnumRemoveFlags(Enum& Flags, Enum FlagsToRemove)
{
    using UnderlyingType = __underlying_type(Enum);
    Flags = (Enum)((UnderlyingType)Flags & ~(UnderlyingType)FlagsToRemove);
}
