#pragma once

#include <vulkan/vulkan.h>

// Every entry point the RHI calls, named without the vk prefix. A new call needs its name added to the list of its dispatch level.
#define LUMINA_VK_GLOBAL_FUNCTIONS(X) \
    X(CreateInstance) \
    X(EnumerateInstanceExtensionProperties) \
    X(EnumerateInstanceLayerProperties) \
    X(EnumerateInstanceVersion)

#define LUMINA_VK_INSTANCE_FUNCTIONS(X) \
    X(CreateDebugUtilsMessengerEXT) \
    X(CreateDevice) \
    X(DestroyDebugUtilsMessengerEXT) \
    X(DestroyInstance) \
    X(DestroySurfaceKHR) \
    X(EnumerateDeviceExtensionProperties) \
    X(EnumeratePhysicalDevices) \
    X(GetDeviceProcAddr) \
    X(GetPhysicalDeviceFeatures2) \
    X(GetPhysicalDeviceFormatProperties2) \
    X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceMemoryProperties2) \
    X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceProperties2) \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) \
    X(GetPhysicalDeviceSurfacePresentModesKHR)

#define LUMINA_VK_DEVICE_FUNCTIONS(X) \
    X(AcquireNextImageKHR) \
    X(AllocateCommandBuffers) \
    X(AllocateDescriptorSets) \
    X(AllocateMemory) \
    X(BeginCommandBuffer) \
    X(BindBufferMemory) \
    X(BindImageMemory) \
    X(CmdBeginDebugUtilsLabelEXT) \
    X(CmdBeginRendering) \
    X(CmdBindDescriptorSets) \
    X(CmdBindIndexBuffer) \
    X(CmdBindIndexBuffer3KHR) \
    X(CmdBindPipeline) \
    X(CmdBlitImage) \
    X(CmdClearColorImage) \
    X(CmdClearDepthStencilImage) \
    X(CmdCopyBuffer) \
    X(CmdCopyBufferToImage) \
    X(CmdCopyImage) \
    X(CmdCopyImageToBuffer) \
    X(CmdCopyMemoryKHR) \
    X(CmdDispatch) \
    X(CmdDispatchIndirect) \
    X(CmdDispatchIndirect2KHR) \
    X(CmdDraw) \
    X(CmdDrawIndexed) \
    X(CmdDrawIndexedIndirect) \
    X(CmdDrawIndexedIndirect2KHR) \
    X(CmdDrawIndirect) \
    X(CmdDrawIndirect2KHR) \
    X(CmdDrawIndirectCount) \
    X(CmdDrawIndirectCount2KHR) \
    X(CmdDrawMeshTasksEXT) \
    X(CmdDrawMeshTasksIndirect2EXT) \
    X(CmdDrawMeshTasksIndirectCount2EXT) \
    X(CmdDrawMeshTasksIndirectCountEXT) \
    X(CmdDrawMeshTasksIndirectEXT) \
    X(CmdEndDebugUtilsLabelEXT) \
    X(CmdEndRendering) \
    X(CmdFillBuffer) \
    X(CmdFillMemoryKHR) \
    X(CmdPipelineBarrier2) \
    X(CmdPushConstants) \
    X(CmdResetQueryPool) \
    X(CmdResolveImage) \
    X(CmdSetCheckpointNV) \
    X(CmdSetCullMode) \
    X(CmdSetDepthBias) \
    X(CmdSetDepthBounds) \
    X(CmdSetDepthBoundsTestEnable) \
    X(CmdSetDepthCompareOp) \
    X(CmdSetDepthTestEnable) \
    X(CmdSetDepthWriteEnable) \
    X(CmdSetFrontFace) \
    X(CmdSetLineWidth) \
    X(CmdSetScissorWithCount) \
    X(CmdSetStencilCompareMask) \
    X(CmdSetStencilOp) \
    X(CmdSetStencilReference) \
    X(CmdSetStencilTestEnable) \
    X(CmdSetStencilWriteMask) \
    X(CmdSetViewportWithCount) \
    X(CmdUpdateBuffer) \
    X(CmdUpdateMemoryKHR) \
    X(CmdWriteBufferMarkerAMD) \
    X(CmdWriteTimestamp2) \
    X(CopyMemoryToImage) \
    X(CreateBuffer) \
    X(CreateCommandPool) \
    X(CreateComputePipelines) \
    X(CreateDescriptorPool) \
    X(CreateDescriptorSetLayout) \
    X(CreateFence) \
    X(CreateGraphicsPipelines) \
    X(CreateImage) \
    X(CreateImageView) \
    X(CreatePipelineCache) \
    X(CreatePipelineLayout) \
    X(CreateQueryPool) \
    X(CreateSampler) \
    X(CreateSemaphore) \
    X(CreateShaderModule) \
    X(CreateSwapchainKHR) \
    X(DestroyBuffer) \
    X(DestroyCommandPool) \
    X(DestroyDescriptorPool) \
    X(DestroyDescriptorSetLayout) \
    X(DestroyDevice) \
    X(DestroyFence) \
    X(DestroyImage) \
    X(DestroyImageView) \
    X(DestroyPipeline) \
    X(DestroyPipelineCache) \
    X(DestroyPipelineLayout) \
    X(DestroyQueryPool) \
    X(DestroySampler) \
    X(DestroySemaphore) \
    X(DestroyShaderModule) \
    X(DestroySwapchainKHR) \
    X(DeviceWaitIdle) \
    X(EndCommandBuffer) \
    X(FreeDescriptorSets) \
    X(FreeMemory) \
    X(GetBufferDeviceAddress) \
    X(GetBufferMemoryRequirements) \
    X(GetBufferMemoryRequirements2) \
    X(GetDeviceFaultInfoEXT) \
    X(GetDeviceQueue) \
    X(GetFenceStatus) \
    X(GetImageMemoryRequirements) \
    X(GetImageMemoryRequirements2) \
    X(GetPipelineCacheData) \
    X(GetPipelineExecutableInternalRepresentationsKHR) \
    X(GetPipelineExecutablePropertiesKHR) \
    X(GetPipelineExecutableStatisticsKHR) \
    X(GetQueryPoolResults) \
    X(GetSemaphoreCounterValue) \
    X(GetSwapchainImagesKHR) \
    X(MapMemory) \
    X(QueuePresentKHR) \
    X(QueueSubmit2) \
    X(ResetCommandPool) \
    X(ResetFences) \
    X(ResetQueryPool) \
    X(SetDebugUtilsObjectNameEXT) \
    X(TransitionImageLayout) \
    X(UnmapMemory) \
    X(UpdateDescriptorSets) \
    X(WaitForFences) \
    X(WaitSemaphores)

namespace Lumina::Vulkan
{
    // Entry points resolved from the loader, then the instance, then the device, so device calls skip the loader's trampoline.
    struct FApi
    {
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;

#define LUMINA_VK_DECLARE_ENTRY(Name) PFN_vk##Name Name = nullptr;
        LUMINA_VK_GLOBAL_FUNCTIONS(LUMINA_VK_DECLARE_ENTRY)
        LUMINA_VK_INSTANCE_FUNCTIONS(LUMINA_VK_DECLARE_ENTRY)
        LUMINA_VK_DEVICE_FUNCTIONS(LUMINA_VK_DECLARE_ENTRY)
#undef LUMINA_VK_DECLARE_ENTRY

        // False when the system has no Vulkan loader library or it exports no vkGetInstanceProcAddr.
        bool OpenLoader();

        // Resolves the device-level entries through the instance too, so a call made before the device exists still dispatches.
        void LoadInstance(VkInstance Instance);

        // Replaces the device-level entries with the device's own, keeping the instance ones the device does not expose.
        void LoadDevice(VkDevice Device);

        void CloseLoader();

    private:

        void* Library = nullptr;
    };

    // The live device's entry points, for Vulkan code outside the RHI's own file.
    RUNTIME_API const FApi& GetApi();
}
