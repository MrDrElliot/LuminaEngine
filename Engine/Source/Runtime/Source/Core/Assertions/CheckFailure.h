#pragma once

#include <source_location>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Assert::Detail
{
    // The assertion entry point for headers that cannot include Assert.h, which reaches FString and <stacktrace>.
    [[noreturn]] FORCENOINLINE RUNTIME_API void HandleCheckFailure(const char* Expression, const std::source_location& Location);

    // The same for a bounds check, which reports the two values that failed it rather than only their names.
    [[noreturn]] FORCENOINLINE RUNTIME_API void HandleBoundsFailure(const char* IndexName, uint64 IndexValue,
        const char* Relation, const char* BoundName, uint64 BoundValue, const std::source_location& Location);
}
