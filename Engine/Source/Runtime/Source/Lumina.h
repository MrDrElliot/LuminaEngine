#pragma once

#include "Platform/GenericPlatform.h"

#define LUMINA_VERSION "0.01.0"
#define LUMINA_VERSION_MAJOR 0
#define LUMINA_VERSION_MINOR 01
#define LUMINA_VERSION_PATCH 0
#define LUMINA_VERSION_NUM 0010


// Declares a zero-initialised function-local static and enters the block only on first use.
#define LUMINA_STATIC_HELPER(InType)                                          \
static InType StaticValue = {};                                               \
if (!StaticValue)

// Invalid Index
constexpr auto INDEX_NONE = -1;
