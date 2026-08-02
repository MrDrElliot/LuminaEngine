#pragma once

#include "Core/Module/ModuleInterface.h"

namespace Lumina
{
    class GAMEPLAYEXTRASRUNTIME_API FGameplayExtrasRuntimeModule : public IModuleInterface
    {
    public:
        void StartupModule()  override;
        void ShutdownModule() override;
    };
}
