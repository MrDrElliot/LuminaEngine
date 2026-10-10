#pragma once

#include "ModuleAPI.h"
#include "Containers/Vector.h"
#include "RHI.h"
#include "Lumina.h"

namespace Lumina::RHI::Native
{
    struct FNativeDeviceHandles
    {
        EBackend Backend             = EBackend::Vulkan;
        void*    Instance            = nullptr;   // Vulkan: VkInstance
        void*    PhysicalDevice      = nullptr;   // Vulkan: VkPhysicalDevice
        void*    Device              = nullptr;   // Vulkan: VkDevice
        void*    GraphicsQueue       = nullptr;   // Vulkan: VkQueue (graphics family)
        uint32   GraphicsQueueFamily = Constants::kIndexNoneU32;
        void*    GetInstanceProcAddr = nullptr;   // Vulkan: PFN_vkGetInstanceProcAddr
        void*    GetDeviceProcAddr   = nullptr;   // Vulkan: PFN_vkGetDeviceProcAddr
        uint32   ApiVersion          = 0;         // Vulkan: VK_API_VERSION_*
    };

    // Snapshot the live native handles. Returns an all-null struct if no device exists yet.
    RUNTIME_API FNativeDeviceHandles GetNativeDeviceHandles();

    // The opaque command buffer behind an RHI handle. Vulkan: VkCommandBuffer. Null if no device.
    RUNTIME_API void* GetNativeCommandBuffer(FCmdListH CommandList);

    struct FNativeTexture
    {
        void*  Image  = nullptr;   // VkImage on Vulkan
        void*  View   = nullptr;   // VkImageView over every mip and layer
        uint32 Format = 0;         // VkFormat
        uint32 Layout = 0;         // VkImageLayout the image stays in between passes
    };

    // For an SDK that records its own work against an engine texture, such as an upscaler. All null if the handle is dead.
    RUNTIME_API FNativeTexture GetNativeTexture(FTextureH Texture);
    
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
