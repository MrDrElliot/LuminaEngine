#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"

namespace Lumina::Spirv
{
    // Drops embedded source, line tables and NonSemantic debug info, keeping every instruction a driver executes.
    RUNTIME_API void StripDebugInfo(TVector<uint32>& Words);

    // Names a declared capability the device is never created with, such as Float16, which Pascal lacks; null when none is.
    RUNTIME_API const char* FindForbiddenCapability(TSpan<const uint32> Words);
}
