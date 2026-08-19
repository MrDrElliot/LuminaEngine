#pragma once

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Shaders
{
    /**
     * The ordered VFS directories that hold compilable `.slang`, and that Slang resolves `#include`
     * against: the engine tree, then every enabled plugin's `/Shaders`, then the loaded project's
     * `/Game/Shaders`, then anything a module registered. Engine first so a plugin or game shipping a
     * file of the same name can never shadow it -- shadowing is reported and the collider has to be
     * requested by its full virtual path.
     *
     * Roots that do not exist on disk are skipped, so a packaged build (source stripped, only the
     * compiled cache shipped) reports none.
     */
    RUNTIME_API void GetSearchRoots(TVector<FString>& OutRoots);

    /** Adds a root for a module or tool shipping shaders outside the standard mounts; the path is a VFS
     *  directory ("/MyMount/Shaders"). Idempotent, and safe after startup: on-demand lookups see it
     *  immediately and the next PrecompileNewRoots() batch-compiles it. */
    RUNTIME_API void RegisterSearchRoot(FStringView VirtualRoot);
    RUNTIME_API void UnregisterSearchRoot(FStringView VirtualRoot);

    /** Maps a shader name ("GameOfLife.slang", "Sub/Foo.slang") onto the first search root holding it. A
     *  path that already names its root is returned as-is when it exists. Empty when nothing matches. */
    RUNTIME_API FString Resolve(FStringView NameOrPath);

    /**
     * Compiles and commits every `.slang` directly under each search root not yet enumerated, and
     * returns how many were submitted (the compile itself is async).
     *
     * Called once from the shader compiler's Initialize -- where only the engine tree and engine
     * plugins are mounted -- and again after a project loads, which is when `/Game` and the project's
     * own plugins appear. Roots are remembered, so the second pass only picks up what is new.
     */
    RUNTIME_API uint32 PrecompileNewRoots();
}
