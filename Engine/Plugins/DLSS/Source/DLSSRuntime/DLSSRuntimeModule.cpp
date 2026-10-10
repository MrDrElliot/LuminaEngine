#include "DLSSRuntimeModule.h"

#include "Core/Delegates/CoreDelegates.h"
#include "Core/Module/ModuleManager.h"
#include "DLSSUpscaler.h"
#include "Log/Log.h"
#include "Renderer/RHINative.h"

#include <volk/volk.h>
#include <nvsdk_ngx_vk.h>

#include <cstring>

using namespace Lumina;

IMPLEMENT_MODULE(FDLSSRuntimeModule, "DLSSRuntime");

void FDLSSRuntimeModule::StartupModule()
{
    uint32 InstanceCount = 0;
    uint32 DeviceCount   = 0;
    const char** InstanceExtensions = nullptr;
    const char** DeviceExtensions   = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_RequiredExtensions(&InstanceCount, &InstanceExtensions, &DeviceCount, &DeviceExtensions)))
    {
        RHI::Native::FDeviceCreationRequest Request;
        for (uint32 Index = 0; Index < InstanceCount; ++Index)
        {
            Request.InstanceExtensions.push_back(InstanceExtensions[Index]);
        }
        for (uint32 Index = 0; Index < DeviceCount; ++Index)
        {
            // Core since Vulkan 1.2, and asking for the EXT form beside the core feature is a validation error.
            if (std::strcmp(DeviceExtensions[Index], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) != 0)
            {
                Request.DeviceExtensions.push_back(DeviceExtensions[Index]);
            }
        }
        RHI::Native::RegisterDeviceCreationRequest(Request);
        LOG_INFO("[DLSS] Requested {} instance and {} device extension(s) for NGX.",
            (int32)Request.InstanceExtensions.size(), (int32)Request.DeviceExtensions.size());
    }

    Upscaler = MakeUnique<FDLSSUpscaler>();
    FUpscalerRegistry::Register(Upscaler.Get());

    ShutdownStartedHandle = FCoreDelegates::OnEngineShutdownStarted.AddLambda([this]()
    {
        ShutdownBeforeDevice();
    });
}

void FDLSSRuntimeModule::ShutdownBeforeDevice()
{
    if (Upscaler == nullptr)
    {
        return;
    }
    FUpscalerRegistry::Unregister(Upscaler.Get());
    Upscaler->Shutdown();
}

void FDLSSRuntimeModule::ShutdownModule()
{
    FCoreDelegates::OnEngineShutdownStarted.Remove(ShutdownStartedHandle);

    // A plugin unloaded mid-session still has a device to release NGX against.
    ShutdownBeforeDevice();
    Upscaler.Reset();
}
