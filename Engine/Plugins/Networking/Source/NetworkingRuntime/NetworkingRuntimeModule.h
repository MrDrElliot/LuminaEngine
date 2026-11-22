#pragma once

#include "Core/Module/ModuleInterface.h"

namespace Lumina
{
    // Installs Lumina's netcode as INetworkRuntime, loading Earliest since FEngine::Initialize drives it before later phases.
    class FNetworkingRuntimeModule : public IModuleInterface
    {
    public:

        void StartupModule() override;
        void ShutdownModule() override;
    };
}
