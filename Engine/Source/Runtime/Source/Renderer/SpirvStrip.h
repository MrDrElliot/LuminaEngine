#pragma once

#include "Containers/Vector.h"

namespace Lumina::Spirv
{
    // Drops embedded source, line tables and NonSemantic debug info, keeping every instruction a driver executes.
    RUNTIME_API void StripDebugInfo(TVector<uint32>& Words);
}
