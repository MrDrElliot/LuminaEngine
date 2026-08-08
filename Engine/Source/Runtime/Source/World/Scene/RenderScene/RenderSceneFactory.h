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

        /** True if a project/plugin renderer is installed. */
        RUNTIME_API bool HasOverride();

        /** The installed factory, or null. Lets an installer check whether the current override is its own. */
        RUNTIME_API FCreateFn GetOverride();

        /** Create a renderer for World: the installed override, or the engine default. Never null. */
        RUNTIME_API TUniquePtr<IRenderScene> Create(CWorld* World);
    }
}
