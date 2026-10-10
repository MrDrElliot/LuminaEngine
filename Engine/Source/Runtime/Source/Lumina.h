#pragma once

#include "Platform/GenericPlatform.h"

#define LUMINA_VERSION_MAJOR 0
#define LUMINA_VERSION_MINOR 1
#define LUMINA_VERSION_PATCH 53

#define LUMINA_VERSION_TEXT_DETAIL(x) #x
#define LUMINA_VERSION_TEXT(x) LUMINA_VERSION_TEXT_DETAIL(x)
#define LUMINA_VERSION LUMINA_VERSION_TEXT(LUMINA_VERSION_MAJOR) "." LUMINA_VERSION_TEXT(LUMINA_VERSION_MINOR) "." LUMINA_VERSION_TEXT(LUMINA_VERSION_PATCH)
#define LUMINA_VERSION_NUM (LUMINA_VERSION_MAJOR * 10000 + LUMINA_VERSION_MINOR * 100 + LUMINA_VERSION_PATCH)


// Declares a zero-initialized function-local static and enters the block only on first use.
#define LUMINA_STATIC_HELPER(InType)                                          \
static InType StaticValue = {};                                               \
if (!StaticValue)

[[deprecated("Use Lumina::Constants::kIndexNone instead.")]]
constexpr auto INDEX_NONE = -1;

namespace Lumina::Constants
{
    // Invalid Index
    constexpr auto kIndexNone = -1;

    // The unsigned "no index" sentinel, for slots, handles and offsets stored as uint32.
    constexpr uint32 kIndexNoneU32 = ~0u;

    // Pinned rather than std::hardware_destructive_interference_size, which differs between compilers and would move every CACHE_ALIGN layout.
    constexpr SIZE_T kCacheLineSize = 64;

    constexpr uint64 kKiB = 1024;
    constexpr uint64 kMiB = 1024 * kKiB;
    constexpr uint64 kGiB = 1024 * kMiB;
}
