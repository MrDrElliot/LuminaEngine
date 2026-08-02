#pragma once

#include "Core/Module/ModuleInterface.h"

namespace Lumina
{
    // Loads at the Core phase (before RHI device creation) so it can register the Vulkan instance/
    // device extensions the Nsight Perf periodic sampler requires, then contributes a Tools-menu
    // entry that opens the live GPU HUD. Editor-only; toggled per-project via .lproject.
    class NSIGHTPERFEDITOR_API FNsightPerfEditorModule : public IModuleInterface
    {
    public:
        void StartupModule()  override;
        void ShutdownModule() override;

    private:
        // Our Tools-menu entry's handle. Must be unregistered in ShutdownModule (before this DLL
        // unloads), its callbacks are code in this DLL, but the registry outlives the plugin.
        uint32 ToolsMenuHandle = 0;
    };
}
