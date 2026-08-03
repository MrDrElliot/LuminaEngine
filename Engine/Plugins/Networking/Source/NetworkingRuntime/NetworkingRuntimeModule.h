#pragma once

#include "Core/Module/ModuleInterface.h"

namespace Lumina
{
    // Installs Lumina's netcode as the engine's INetworkRuntime. Loading at the Earliest phase because
    // FEngine::Initialize drives INetworkRuntime::Initialize before any later phase has run.
    class FNetworkingRuntimeModule : public IModuleInterface
    {
    public:

        void StartupModule() override;
        void ShutdownModule() override;
    };
}
