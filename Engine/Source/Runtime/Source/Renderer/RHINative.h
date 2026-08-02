#pragma once

#include "ModuleAPI.h"
#include "Containers/Array.h"
#include "RHI.h"

namespace Lumina::RHI::Native
{
    struct FNativeDeviceHandles
    {
        EBackend Backend             = EBackend::Vulkan;
        void*    Instance            = nullptr;   // Vulkan: VkInstance
        void*    PhysicalDevice      = nullptr;   // Vulkan: VkPhysicalDevice
        void*    Device              = nullptr;   // Vulkan: VkDevice
        void*    GraphicsQueue       = nullptr;   // Vulkan: VkQueue (graphics family)
        uint32   GraphicsQueueFamily = ~0u;
        void*    GetInstanceProcAddr = nullptr;   // Vulkan: PFN_vkGetInstanceProcAddr
        void*    GetDeviceProcAddr   = nullptr;   // Vulkan: PFN_vkGetDeviceProcAddr
        uint32   ApiVersion          = 0;         // Vulkan: VK_API_VERSION_*
    };

    // Snapshot the live native handles. Returns an all-null struct if no device exists yet.
    RUNTIME_API FNativeDeviceHandles GetNativeDeviceHandles();

    // The opaque command buffer behind an RHI handle. Vulkan: VkCommandBuffer. Null if no device.
    RUNTIME_API void* GetNativeCommandBuffer(FCmdListH CommandList);
    
    struct FDeviceCreationRequest
    {
        TVector<const char*> InstanceExtensions;
        TVector<const char*> DeviceExtensions;
        void*                DeviceCreatePNext = nullptr;
    };
    
    RUNTIME_API void RegisterDeviceCreationRequest(const FDeviceCreationRequest& Request);
    
    RUNTIME_API void AcquireSubmitLock();
    RUNTIME_API void ReleaseSubmitLock();

    // RAII form of AcquireSubmitLock/ReleaseSubmitLock.
    struct FScopedSubmitLock
    {
        FScopedSubmitLock()  { AcquireSubmitLock(); }
        ~FScopedSubmitLock() { ReleaseSubmitLock(); }
        FScopedSubmitLock(const FScopedSubmitLock&) = delete;
        FScopedSubmitLock& operator=(const FScopedSubmitLock&) = delete;
    };
}
