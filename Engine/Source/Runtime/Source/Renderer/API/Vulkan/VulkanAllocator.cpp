#include "RuntimePCH.h"
#include "VulkanAllocator.h"

#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"

namespace Lumina::Vulkan
{
    namespace
    {
        VKAPI_ATTR void* VKAPI_CALL HostAllocate(void*, size_t Size, size_t Alignment, VkSystemAllocationScope)
        {
            LUMINA_MEMORY_SCOPE("Vulkan");
            return Memory::Malloc(Size, Alignment);
        }

        VKAPI_ATTR void* VKAPI_CALL HostReallocate(void*, void* Original, size_t Size, size_t Alignment, VkSystemAllocationScope)
        {
            LUMINA_MEMORY_SCOPE("Vulkan");
            if (Original == nullptr)
            {
                return Memory::Malloc(Size, Alignment);
            }

            // The spec makes a zero size a free that returns null.
            if (Size == 0)
            {
                Memory::Free(Original);
                return nullptr;
            }

            return Memory::Realloc(Original, Size, Alignment);
        }

        VKAPI_ATTR void VKAPI_CALL HostFree(void*, void* Block)
        {
            if (Block != nullptr)
            {
                Memory::Free(Block);
            }
        }

        constexpr VkAllocationCallbacks GHostAllocator
        {
            .pUserData             = nullptr,
            .pfnAllocation         = &HostAllocate,
            .pfnReallocation       = &HostReallocate,
            .pfnFree               = &HostFree,
            .pfnInternalAllocation = nullptr,
            .pfnInternalFree       = nullptr,
        };
    }

    const VkAllocationCallbacks* HostAllocator()
    {
        return &GHostAllocator;
    }
}
