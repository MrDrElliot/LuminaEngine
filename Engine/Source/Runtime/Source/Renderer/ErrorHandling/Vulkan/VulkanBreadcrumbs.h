#pragma once

#include <atomic>
#include <volk/volk.h>

#include "Containers/String.h"

namespace Lumina::RHI
{
    class FGpuBreadcrumbs
    {
    public:

        static constexpr uint32 MaxMarkers   = 2048;
        static constexpr uint32 MaxNameChars = 64;
        static constexpr uint32 MaxDepth     = 32;
        static constexpr uint32 InvalidIndex = ~0u;

        bool Initialize(VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
        void Shutdown(VkDevice InDevice);

        NODISCARD bool IsEnabled() const { return Mapped != nullptr; }

        // Records a begin marker. Returns the index to hand to End, or InvalidIndex when disabled.
        uint32 Begin(VkCommandBuffer Cmd, const char* Name, uint32 Depth);

        void End(VkCommandBuffer Cmd, uint32 Index);

        FString ReportOutstanding() const;

    private:

        struct FEntry
        {
            char   Name[MaxNameChars];
            uint32 MarkerId;
            uint32 Depth;
        };

        VkBuffer         Buffer = VK_NULL_HANDLE;
        VkDeviceMemory   Memory = VK_NULL_HANDLE;

        volatile uint32* Mapped = nullptr;

        FEntry Entries[MaxMarkers] = {};

        std::atomic<uint32> NextMarkerId{ 1 };

        // Mutable so ReportOutstanding can stay const; it is logically a read.
        mutable std::atomic<bool> bReported{ false };
    };
}
