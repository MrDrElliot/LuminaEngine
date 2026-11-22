#pragma once

#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina
{
    // A cook reachability entry point, since the cooker drops anything unreachable from every root.
    struct FCookRoot
    {
        // VFS path; ".lasset" suffix optional (resolver handles either).
        FString Asset;

        // Chunk the asset + hard-reachable children land in (recorded now, routed for per-chunk emission later).
        FName   Chunk;
    };
}
