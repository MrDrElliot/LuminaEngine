#pragma once

#include "Core/Module/ModuleInterface.h"
#include "renderdoc_app.h"

namespace Lumina
{
    // Loads at the Core phase because RenderDoc only hooks a Vulkan instance created after it is in the process.
    class RENDERDOCEDITOR_API FRenderDocEditorModule : public IModuleInterface
    {
    public:
        void StartupModule()  override;
        void ShutdownModule() override;

    private:
        void CaptureNextFrame() const;

        RENDERDOC_API_1_6_0* RenderDocAPI = nullptr;

        // Must be unregistered in ShutdownModule since its callbacks live in this DLL but the registry outlives the plugin.
        uint32 ToolsMenuHandle = 0;
    };
}
