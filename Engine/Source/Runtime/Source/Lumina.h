#pragma once

#include "Platform/GenericPlatform.h"

#define LUMINA_VERSION_MAJOR 0
#define LUMINA_VERSION_MINOR 1
#define LUMINA_VERSION_PATCH 21

#define LUMINA_VERSION_TEXT_DETAIL(x) #x
#define LUMINA_VERSION_TEXT(x) LUMINA_VERSION_TEXT_DETAIL(x)
#define LUMINA_VERSION LUMINA_VERSION_TEXT(LUMINA_VERSION_MAJOR) "." LUMINA_VERSION_TEXT(LUMINA_VERSION_MINOR) "." LUMINA_VERSION_TEXT(LUMINA_VERSION_PATCH)
#define LUMINA_VERSION_NUM (LUMINA_VERSION_MAJOR * 10000 + LUMINA_VERSION_MINOR * 100 + LUMINA_VERSION_PATCH)


// Declares a zero-initialized function-local static and enters the block only on first use.
#define LUMINA_STATIC_HELPER(InType)                                          \
static InType StaticValue = {};                                               \
if (!StaticValue)

// Invalid Index
constexpr auto INDEX_NONE = -1;
