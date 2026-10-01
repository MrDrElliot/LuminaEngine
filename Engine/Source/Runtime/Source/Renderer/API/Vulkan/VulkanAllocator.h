#pragma once

#include <volk/volk.h>

namespace Lumina::Vulkan
{
    // Host memory the loader, driver and VMA ask for, served by the engine allocator. Pass it to every create and destroy.
    const VkAllocationCallbacks* HostAllocator();
}
