#pragma once

#include "Core/Delegates/Delegate.h"
#include "Core/Module/ModuleInterface.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FDLSSUpscaler;

    // Loads before the Vulkan device exists so the extensions NGX needs are enabled on it, then offers DLSS as an upscaler.
    class DLSSRUNTIME_API FDLSSRuntimeModule : public IModuleInterface
    {
    public:

        void StartupModule() override;
        void ShutdownModule() override;

    private:

        // NGX has to go before the device does, and plugins shut down after the renderer has destroyed it.
        void ShutdownBeforeDevice();

        TUniquePtr<FDLSSUpscaler> Upscaler;
        FDelegateHandle           ShutdownStartedHandle;
    };
}
