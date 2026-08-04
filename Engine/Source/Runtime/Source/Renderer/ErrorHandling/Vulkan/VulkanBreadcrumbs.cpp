#include "RuntimePCH.h"
#include "VulkanBreadcrumbs.h"

#include <cstring>

#include "Log/Log.h"

namespace Lumina::RHI
{
    namespace
    {
        constexpr VkDeviceSize GBufferBytes =
            static_cast<VkDeviceSize>(FGpuBreadcrumbs::MaxMarkers) * 2 * sizeof(uint32);

        // Host visible and coherent, and explicitly NOT device local.
        //
        // The exclusion is the whole point. On a resizable-BAR system there is a memory type that is
        // both HOST_VISIBLE and DEVICE_LOCAL, and VMA or a naive first-match search will happily
        // pick it -- but that memory lives on the card, so a device loss takes the breadcrumbs with
        // it and the buffer reads back as zeroes exactly when it matters.
        uint32 FindHostMemoryType(VkPhysicalDevice PhysicalDevice, uint32 TypeBits)
        {
            VkPhysicalDeviceMemoryProperties MemProps{};
            vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &MemProps);

            constexpr VkMemoryPropertyFlags Required =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            for (uint32 Index = 0; Index < MemProps.memoryTypeCount; ++Index)
            {
                if ((TypeBits & (1u << Index)) == 0)
                {
                    continue;
                }

                const VkMemoryPropertyFlags Flags = MemProps.memoryTypes[Index].propertyFlags;

                if ((Flags & Required) == Required
                    && (Flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)
                {
                    return Index;
                }
            }

            return ~0u;
        }
    }


    bool FGpuBreadcrumbs::Initialize(VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    {
        if (vkCmdWriteBufferMarkerAMD == nullptr)
        {
            LOG_WARN("GPU breadcrumbs unavailable: VK_AMD_buffer_marker is not supported by this driver. "
                     "A device loss will not be able to say which pass was executing.");
            return false;
        }

        VkBufferCreateInfo BufferInfo{};
        BufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        BufferInfo.size        = GBufferBytes;
        BufferInfo.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(InDevice, &BufferInfo, nullptr, &Buffer) != VK_SUCCESS)
        {
            LOG_WARN("GPU breadcrumbs disabled: buffer creation failed.");
            return false;
        }

        VkMemoryRequirements Requirements{};
        vkGetBufferMemoryRequirements(InDevice, Buffer, &Requirements);

        const uint32 MemoryType = FindHostMemoryType(InPhysicalDevice, Requirements.memoryTypeBits);
        if (MemoryType == ~0u)
        {
            LOG_WARN("GPU breadcrumbs disabled: no host-visible, non-device-local memory type available.");
            Shutdown(InDevice);
            return false;
        }

        VkMemoryAllocateInfo AllocInfo{};
        AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        AllocInfo.allocationSize  = Requirements.size;
        AllocInfo.memoryTypeIndex = MemoryType;

        if (vkAllocateMemory(InDevice, &AllocInfo, nullptr, &Memory) != VK_SUCCESS)
        {
            LOG_WARN("GPU breadcrumbs disabled: host memory allocation failed.");
            Shutdown(InDevice);
            return false;
        }

        if (vkBindBufferMemory(InDevice, Buffer, Memory, 0) != VK_SUCCESS)
        {
            LOG_WARN("GPU breadcrumbs disabled: buffer bind failed.");
            Shutdown(InDevice);
            return false;
        }

        void* Address = nullptr;
        if (vkMapMemory(InDevice, Memory, 0, VK_WHOLE_SIZE, 0, &Address) != VK_SUCCESS)
        {
            LOG_WARN("GPU breadcrumbs disabled: memory map failed.");
            Shutdown(InDevice);
            return false;
        }

        // Mapped for the life of the process and never unmapped, so the crash path can read it
        // without calling into a driver whose device has just gone away.
        std::memset(Address, 0, static_cast<size_t>(GBufferBytes));
        Mapped = static_cast<volatile uint32*>(Address);

        LOG_DISPLAY("GPU breadcrumbs enabled ({} markers, host memory).", MaxMarkers);
        return true;
    }


    void FGpuBreadcrumbs::Shutdown(VkDevice InDevice)
    {
        Mapped = nullptr;

        if (Memory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(InDevice, Memory);
            vkFreeMemory(InDevice, Memory, nullptr);
            Memory = VK_NULL_HANDLE;
        }

        if (Buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(InDevice, Buffer, nullptr);
            Buffer = VK_NULL_HANDLE;
        }
    }


    uint32 FGpuBreadcrumbs::Begin(VkCommandBuffer Cmd, const char* Name, uint32 Depth)
    {
        if (Mapped == nullptr || Cmd == VK_NULL_HANDLE)
        {
            return InvalidIndex;
        }

        // Wraps the ring. Old entries are not cleared: a stale slot is identified by its recorded
        // marker id no longer matching what the GPU wrote, so it filters itself out on readback.
        const uint32 MarkerId = NextMarkerId.fetch_add(1, std::memory_order_relaxed);
        const uint32 Index    = MarkerId % MaxMarkers;

        FEntry& Entry = Entries[Index];

        // Copied, not referenced: the caller's string is frequently a temporary, and the table has
        // to still be readable during the crash long after that has gone.
        const size_t NameLength = Name != nullptr ? std::strlen(Name) : 0;
        const size_t Copied     = NameLength < MaxNameChars ? NameLength : MaxNameChars - 1;
        std::memcpy(Entry.Name, Name != nullptr ? Name : "", Copied);
        Entry.Name[Copied] = 0;

        Entry.Depth    = Depth;
        Entry.MarkerId = MarkerId;

        // TOP_OF_PIPE: the write lands as the pass is entered, before any of its work runs.
        vkCmdWriteBufferMarkerAMD(Cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Buffer,
            static_cast<VkDeviceSize>(Index) * 2 * sizeof(uint32), MarkerId);

        return Index;
    }


    void FGpuBreadcrumbs::End(VkCommandBuffer Cmd, uint32 Index)
    {
        if (Mapped == nullptr || Cmd == VK_NULL_HANDLE || Index >= MaxMarkers)
        {
            return;
        }

        // BOTTOM_OF_PIPE: only once every stage of the pass has retired. Matching the begin's id is
        // what lets readback pair the two without a second table.
        vkCmdWriteBufferMarkerAMD(Cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Buffer,
            (static_cast<VkDeviceSize>(Index) * 2 + 1) * sizeof(uint32), Entries[Index].MarkerId);
    }


    FString FGpuBreadcrumbs::ReportOutstanding() const
    {
        // Whichever path gets here first wins; the other is the same crash asking twice.
        if (bReported.exchange(true, std::memory_order_acq_rel))
        {
            return {};
        }

        if (Mapped == nullptr)
        {
            LOG_ERROR("[GPU] No breadcrumbs: VK_AMD_buffer_marker was unavailable this run.");
            return {};
        }

        // Recorded count and last-completed marker are reported even when nothing is outstanding.
        // Without them an empty result is ambiguous: an idle GPU at the moment of a CPU crash and a
        // breadcrumb system that never wrote anything produce the identical "none outstanding" line,
        // and those need very different responses.
        const uint32 TotalRecorded = NextMarkerId.load(std::memory_order_relaxed) - 1;

        const FEntry* LastCompleted = nullptr;

        // Collected then sorted by marker id so the log reads in submission order. The GPU is deeply
        // pipelined, so expect several passes outstanding at once rather than a single culprit.
        TVector<const FEntry*> Outstanding;
        Outstanding.reserve(64);

        for (uint32 Index = 0; Index < MaxMarkers; ++Index)
        {
            const FEntry& Entry = Entries[Index];
            if (Entry.MarkerId == 0)
            {
                continue;
            }

            const uint32 BeganValue = Mapped[Index * 2];
            const uint32 EndedValue = Mapped[Index * 2 + 1];

            // Began is this exact marker and ended is not: it was executing when the device died.
            // A stale entry fails the first test; a completed one fails the second.
            if (BeganValue == Entry.MarkerId && EndedValue != Entry.MarkerId)
            {
                Outstanding.push_back(&Entry);
            }
            else if (EndedValue == Entry.MarkerId
                && (LastCompleted == nullptr || Entry.MarkerId > LastCompleted->MarkerId))
            {
                // Newest marker the GPU finished. Proof the trail is live, and on an idle-GPU crash
                // it is the last thing the GPU actually did.
                LastCompleted = &Entry;
            }
        }

        if (Outstanding.empty())
        {
            if (TotalRecorded == 0)
            {
                // Not "the GPU was idle" -- nothing ever recorded a marker. Either no pass has run
                // yet, or CmdBeginMarker is not reaching the breadcrumb ring at all.
                LOG_ERROR("[GPU] Breadcrumbs: no markers were ever recorded this session. The trail "
                          "is not being written, so this says nothing about what the GPU was doing.");
            }
            else if (LastCompleted == nullptr)
            {
                // Recorded but none begun and none ended: the CPU wrote the markers into command
                // buffers the GPU never got to. That is a crash during recording or before submit,
                // NOT an idle GPU -- reporting it as "drained" points at the wrong half of the frame.
                LOG_ERROR("[GPU] Breadcrumbs: {} marker(s) recorded but the GPU executed none of them. "
                          "The crash happened while building the frame, before any of this work ran.",
                    TotalRecorded);
            }
            else
            {
                LOG_ERROR("[GPU] Breadcrumbs: {} marker(s) recorded, none outstanding -- the GPU had "
                          "drained, so the fault was not inside a marked pass. Last completed: {} (#{}).",
                    TotalRecorded,
                    LastCompleted != nullptr ? LastCompleted->Name : "<unknown>",
                    LastCompleted != nullptr ? LastCompleted->MarkerId : 0u);
            }

            return {};
        }

        eastl::sort(Outstanding.begin(), Outstanding.end(),
            [](const FEntry* A, const FEntry* B)
            {
                return A->MarkerId < B->MarkerId;
            });

        LOG_ERROR("[GPU] GPU breadcrumbs: {} pass(es) in flight when the device was lost "
                  "(submission order, deepest nesting last):", (uint32)Outstanding.size());

        for (const FEntry* Entry : Outstanding)
        {
            // Indented by recorded nesting depth, so the list reads as the pass tree it came from.
            FString Indent;
            for (uint32 i = 0; i < Entry->Depth && i < MaxDepth; ++i)
            {
                Indent += "  ";
            }

            LOG_ERROR("[GPU]   {}> {} (#{})", Indent.c_str(), Entry->Name, Entry->MarkerId);
        }

        // The last one begun is the innermost still-running pass, which is the single most useful
        // thing to see without opening the log.
        return FString(Outstanding.back()->Name);
    }
}
