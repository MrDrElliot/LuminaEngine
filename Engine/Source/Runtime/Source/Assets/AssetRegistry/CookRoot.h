#pragma once

#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina
{
    // A cook reachability entry point; the cooker BFS's from every root and drops anything unreachable.
    // Sourced from .lproject + plugin "CookRoots" arrays (+ a legacy GameStartupMap shim).
    struct FCookRoot
    {
        // VFS path; ".lasset" suffix optional (resolver handles either).
        FString Asset;

        // Chunk the asset + hard-reachable children land in (recorded now, routed for per-chunk emission later).
        FName   Chunk;
    };
}
