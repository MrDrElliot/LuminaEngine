#pragma once
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    class CWorld;
    class IRenderScene;

    /**
     * Creation point for every world renderer. By default the engine creates its clustered
     * forward renderer (FForwardRenderScene); a project or plugin can replace it process-wide
     * by installing an override from its module startup:
     *
     *     void FMyGameModule::StartupModule()
     *     {
     *         RenderSceneFactory::SetOverride([](CWorld* World) -> TUniquePtr<IRenderScene>
     *         {
     *             return MakeUnique<FMyRenderScene>(World);
     *         }, "MyRenderScene");
     *     }
     *
     *     void FMyGameModule::ShutdownModule()
     *     {
     *         RenderSceneFactory::SetOverride(nullptr);
     *     }
     *
     * The override applies to worlds created after the call; live worlds keep the renderer they
     * were created with. A module that installs an override MUST clear it before unloading --
     * the factory pointer would otherwise dangle into the unloaded DLL.
     */
    namespace RenderSceneFactory
    {
        using FCreateFn = TUniquePtr<IRenderScene> (*)(CWorld* World);

        /** Install (or with nullptr, remove) the process-wide renderer override. DebugName is logged.
         *  The factory may return null to decline a world (e.g. editor/utility worlds); that world then
         *  gets the engine default renderer. */
        RUNTIME_API void SetOverride(FCreateFn Factory, const char* DebugName = nullptr);

        /** True if a project/plugin renderer is installed. */
        RUNTIME_API bool HasOverride();

        /** The installed factory, or null. Lets an installer check whether the current override is its own. */
        RUNTIME_API FCreateFn GetOverride();

        /** Create a renderer for World: the installed override, or the engine default. Never null. */
        RUNTIME_API TUniquePtr<IRenderScene> Create(CWorld* World);
    }
}
