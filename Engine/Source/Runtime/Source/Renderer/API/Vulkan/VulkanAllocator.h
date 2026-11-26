#pragma once

#include <vulkan/vulkan.h>

namespace Lumina::Vulkan
{
    // Host memory the loader and driver ask for, served by the engine allocator. Pass it to every create and destroy.
    const VkAllocationCallbacks* HostAllocator();
}
