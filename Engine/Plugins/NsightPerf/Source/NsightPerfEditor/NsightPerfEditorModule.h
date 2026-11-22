#pragma once

#include "Core/Module/ModuleInterface.h"

namespace Lumina
{
    // Loads at the Core phase before RHI device creation so it can register the Vulkan extensions the Nsight Perf sampler requires.
    class NSIGHTPERFEDITOR_API FNsightPerfEditorModule : public IModuleInterface
    {
    public:
        void StartupModule()  override;
        void ShutdownModule() override;

    private:
        // Must be unregistered in ShutdownModule since its callbacks live in this DLL but the registry outlives the plugin.
        uint32 ToolsMenuHandle = 0;
    };
}
