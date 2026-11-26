#include "RuntimePCH.h"
#include "VulkanLoader.h"

#include "Platform/Process/PlatformProcess.h"

namespace Lumina::Vulkan
{
    namespace
    {
#if defined(LE_PLATFORM_WINDOWS)
        constexpr const char* kLoaderLibrary = "vulkan-1.dll";
#else
        constexpr const char* kLoaderLibrary = "libvulkan.so.1";
#endif
    }

    bool FApi::OpenLoader()
    {
        Library = Platform::LoadLibraryWithSearchPaths(FString(kLoaderLibrary), {});
        if (Library == nullptr)
        {
            return false;
        }

        GetInstanceProcAddr = Platform::LumGetProcAddress<PFN_vkGetInstanceProcAddr>(Library, "vkGetInstanceProcAddr");
        if (GetInstanceProcAddr == nullptr)
        {
            CloseLoader();
            return false;
        }

#define LUMINA_VK_LOAD_GLOBAL(Name) Name = reinterpret_cast<PFN_vk##Name>(GetInstanceProcAddr(VK_NULL_HANDLE, "vk" #Name));
        LUMINA_VK_GLOBAL_FUNCTIONS(LUMINA_VK_LOAD_GLOBAL)
#undef LUMINA_VK_LOAD_GLOBAL
        return CreateInstance != nullptr;
    }

    void FApi::LoadInstance(VkInstance Instance)
    {
#define LUMINA_VK_LOAD_INSTANCE(Name) Name = reinterpret_cast<PFN_vk##Name>(GetInstanceProcAddr(Instance, "vk" #Name));
        LUMINA_VK_INSTANCE_FUNCTIONS(LUMINA_VK_LOAD_INSTANCE)
        LUMINA_VK_DEVICE_FUNCTIONS(LUMINA_VK_LOAD_INSTANCE)
#undef LUMINA_VK_LOAD_INSTANCE
    }

    void FApi::LoadDevice(VkDevice Device)
    {
#define LUMINA_VK_LOAD_DEVICE(Name) \
        if (const PFN_vkVoidFunction Resolved = GetDeviceProcAddr(Device, "vk" #Name)) { Name = reinterpret_cast<PFN_vk##Name>(Resolved); }
        LUMINA_VK_DEVICE_FUNCTIONS(LUMINA_VK_LOAD_DEVICE)
#undef LUMINA_VK_LOAD_DEVICE
    }

    void FApi::CloseLoader()
    {
        if (Library != nullptr)
        {
            Platform::FreeDLLHandle(Library);
            Library = nullptr;
        }
        *this = FApi{};
    }
}
