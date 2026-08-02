#include "RuntimePCH.h"
#include "LuminaModule.h"

#include "Core/Module/ModuleManager.h"
#include "Memory/Memory.h"

namespace Lumina
{
    void FLuminaModule::StartupModule()
    {
        IModuleInterface::StartupModule();
    }

    void FLuminaModule::ShutdownModule()
    {
        IModuleInterface::ShutdownModule();
    }

}

IMPLEMENT_MODULE(Lumina::FLuminaModule, "LuminaModule");
