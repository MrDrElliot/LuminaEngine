#pragma once
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CWorld;
    class IRenderScene;

    namespace RenderSceneFactory
    {
        using FCreateFn = TUniquePtr<IRenderScene> (*)(CWorld* World);

        RUNTIME_API void SetOverride(FCreateFn Factory, const char* DebugName = nullptr);

        // True if a project or plugin renderer is installed.
        RUNTIME_API bool HasOverride();

        // The installed factory or null, so an installer can tell whether the current override is its own.
        RUNTIME_API FCreateFn GetOverride();

        // Creates the installed override for World, or the engine default, and never returns null.
        RUNTIME_API TUniquePtr<IRenderScene> Create(CWorld* World);
    }
}
