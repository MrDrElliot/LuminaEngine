#pragma once

#include "Platform/GenericPlatform.h"

#define LUMINA_VERSION_MAJOR 0
#define LUMINA_VERSION_MINOR 1
#define LUMINA_VERSION_PATCH 185

#define LUMINA_VERSION_TEXT_DETAIL(x) #x
#define LUMINA_VERSION_TEXT(x) LUMINA_VERSION_TEXT_DETAIL(x)
#define LUMINA_VERSION LUMINA_VERSION_TEXT(LUMINA_VERSION_MAJOR) "." LUMINA_VERSION_TEXT(LUMINA_VERSION_MINOR) "." LUMINA_VERSION_TEXT(LUMINA_VERSION_PATCH)
#define LUMINA_VERSION_NUM (LUMINA_VERSION_MAJOR * 10000 + LUMINA_VERSION_MINOR * 100 + LUMINA_VERSION_PATCH)

namespace Lumina::Constants
{
    // Pinned rather than std::hardware_destructive_interference_size, which differs between compilers and would move every CACHE_ALIGN layout.
    constexpr SIZE_T kCacheLineSize = 64;

    constexpr uint64 kKiB = 1024;
    constexpr uint64 kMiB = 1024 * kKiB;
    constexpr uint64 kGiB = 1024 * kMiB;
}
