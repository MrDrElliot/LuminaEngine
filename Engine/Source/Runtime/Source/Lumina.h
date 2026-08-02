#pragma once

#include "Platform/GenericPlatform.h"

#define LUMINA_VERSION "0.01.0"
#define LUMINA_VERSION_MAJOR 0
#define LUMINA_VERSION_MINOR 01
#define LUMINA_VERSION_PATCH 0
#define LUMINA_VERSION_NUM 0010


// Pick an unsigned integer type big enough to hold N bytes
template <size_t N>
struct TBytesToUnsigned;

template <> struct TBytesToUnsigned<1> { using Type = uint8;  };
template <> struct TBytesToUnsigned<2> { using Type = uint16; };
template <> struct TBytesToUnsigned<4> { using Type = uint32; };
template <> struct TBytesToUnsigned<8> { using Type = uint64; };

#define LUMINA_STATIC_HELPER(InType)                                          \
static TBytesToUnsigned<sizeof(InType)>::Type StaticRawValue = 0;             \
InType& StaticValue = *reinterpret_cast<InType*>(&StaticRawValue);            \
if (!StaticRawValue)

// Invalid Index
constexpr auto INDEX_NONE = -1;
