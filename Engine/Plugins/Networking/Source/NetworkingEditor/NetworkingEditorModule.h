#pragma once

#include "Core/Module/ModuleInterface.h"

namespace Lumina
{
    // Surfaces the Network tool through FToolsMenuRegistry rather than the editor hard-coding it, so
    // the engine's Editor module carries no reference to netcode tooling.
    class FNetworkingEditorModule : public IModuleInterface
    {
    public:

        void StartupModule() override;
        void ShutdownModule() override;

    private:

        uint32 ToolsMenuHandle = 0;
    };
}
