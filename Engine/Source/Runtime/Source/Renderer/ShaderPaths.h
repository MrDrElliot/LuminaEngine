#pragma once

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Containers/Span.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Shaders
{
    // Engine, plugin, project, then registered shader roots that exist on disk; engine first so a same-named plugin or game file never shadows it.
    RUNTIME_API void GetSearchRoots(TVector<FString>& OutRoots);

    // Idempotent and safe after startup, since lookups see the VFS root at once and the next PrecompileNewRoots compiles it.
    RUNTIME_API void RegisterSearchRoot(FStringView VirtualRoot);
    RUNTIME_API void UnregisterSearchRoot(FStringView VirtualRoot);

    // First search root path holding the shader name, the path itself when it already names an existing root, or empty.
    RUNTIME_API FString Resolve(FStringView NameOrPath);

    // Async-compiles each `.slang` under roots not yet enumerated and returns the count, so the post-project-load call only adds new roots.
    RUNTIME_API uint32 PrecompileNewRoots();

    // Folds every source file under the directories in path order, so a cache keyed on it misses once any of them changes.
    RUNTIME_API uint64 HashSourceDirectories(TSpan<const FStringView> Directories);
}
