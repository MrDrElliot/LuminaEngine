#pragma once

#include "ModuleAPI.h"
#include "Containers/Array.h"
#include "Containers/Name.h"

namespace Lumina
{
    class CStruct;
}

namespace Lumina::PrefabOverride
{
    /** Recursively diff a reflected component instance against its prefab baseline, emitting the
     *  delimiter-joined path ("Top" or "Nested.Field") of every serializable leaf that differs.
     *  Nested structs with reflected fields recurse; dynamic containers and opaque structs (math
     *  types, StructOps-only) are atomic leaves. */
    RUNTIME_API void CollectOverriddenLeaves(CStruct* Struct, const void* Instance, const void* Prefab, TVector<FName>& OutPaths);

    /** Copy prefab -> instance for every serializable leaf whose path is NOT in OverriddenPaths,
     *  preserving overridden leaves. Recurses structs so non-overridden siblings of an overridden
     *  nested field still propagate. */
    RUNTIME_API void ApplyInheritedLeaves(CStruct* Struct, void* Instance, const void* Prefab, const THashSet<FName>& OverriddenPaths);
}
