#pragma once

#include <atomic>
#include <volk/volk.h>

#include "Containers/String.h"

namespace Lumina::RHI
{
    // GPU progress breadcrumbs.
    //
    // Writes a marker value into a host-memory buffer from inside the command stream at every pass
    // boundary. The point of host memory is that it survives the device: when the GPU is lost its
    // own memory goes with it, but writes that already crossed into system RAM are still readable.
    // Anything begun and not ended when the device died was in flight, which is as close to a "GPU
    // callstack" as the hardware offers.
    //
    // Backed by VK_AMD_buffer_marker, which despite the name is implemented by NVIDIA drivers too.
    // Disabled cleanly when the extension is absent (notably Intel) -- every entry point no-ops.
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

        // Logs every marker that began and never ended, innermost last, and returns the innermost
        // one as a one-line summary for the crash report. Safe to call on a dead device: it only
        // reads mapped host memory and touches no Vulkan entry point.
        //
        // One-shot. A lost device reports directly and then dies through the crash handler, which
        // asks every diagnostic provider again; without the guard the trail is logged twice.
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

        // Two uint32 per marker: [Index * 2] is the begin value, [Index * 2 + 1] the end value.
        // Volatile because the writer is the GPU, not this thread.
        volatile uint32* Mapped = nullptr;

        FEntry Entries[MaxMarkers] = {};

        // Starts at 1; 0 means "never written" and the buffer is zeroed at init, so a marker id can
        // never be confused with an untouched slot.
        std::atomic<uint32> NextMarkerId{ 1 };

        // Mutable so ReportOutstanding can stay const; it is logically a read.
        mutable std::atomic<bool> bReported{ false };
    };
}
