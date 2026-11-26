#pragma once

#include "VulkanLoader.h"
#include "Containers/Vector.h"
#include "Core/Threading/Sync.h"
#include "Memory/Allocators/RangeAllocator.h"
#include "Memory/SmartPtr.h"
#include "Renderer/RHI.h"

namespace Lumina::Vulkan
{
    struct FSharedMemoryBlock;

    // What one buffer or image is bound to, all of a VkDeviceMemory or a range of a block images share.
    struct FMemoryAllocation
    {
        VkDeviceMemory           Memory    = VK_NULL_HANDLE;
        uint64                   Offset    = 0;
        uint64                   Size      = 0;
        // The host address of Offset, null for memory the host cannot see.
        void*                    Mapped    = nullptr;
        uint32                   TypeIndex = 0;
        // Null for memory of its own.
        FSharedMemoryBlock*      Block     = nullptr;
        FRangeAllocator::FHandle Range     = FRangeAllocator::kInvalidHandle;

        bool IsValid() const { return Memory != VK_NULL_HANDLE; }
    };

    // One heap as the driver reports it, next to what this allocator holds in it.
    struct FHeapBudget
    {
        uint64 BudgetBytes     = 0;
        uint64 UsageBytes      = 0;
        uint64 BlockBytes      = 0;
        uint64 AllocationBytes = 0;
        uint32 BlockCount      = 0;
        uint32 AllocationCount = 0;
    };

    // Device memory for the RHI. Buffer pages are already large, so each gets memory of its own, and images share blocks unless the driver wants them alone.
    class FDeviceMemoryAllocator
    {
    public:

        FDeviceMemoryAllocator();
        ~FDeviceMemoryAllocator();

        void Initialize(const FApi& InApi, VkPhysicalDevice InPhysicalDevice, VkDevice InDevice, bool bInMemoryBudget, bool bInMemoryPriority);
        void Shutdown();

        // Binds memory of its own, mapped when the type is host visible and addressable from shaders.
        bool AllocateBuffer(VkBuffer Buffer, RHI::EMemoryType Type, FMemoryAllocation& Out);

        // An attachment gets the highest residency priority, since the driver evicting a render target stalls every frame.
        bool AllocateImage(VkImage Image, bool bAttachment, FMemoryAllocation& Out);

        // Any thread, once whatever was bound to the memory is destroyed.
        void Free(FMemoryAllocation& Allocation);

        const VkPhysicalDeviceMemoryProperties& GetProperties() const { return Properties; }
        void GetHeapBudgets(FHeapBudget (&Out)[VK_MAX_MEMORY_HEAPS]) const;

    private:

        struct FCandidates
        {
            uint32 Types[VK_MAX_MEMORY_TYPES];
            uint32 Num = 0;
        };

        FCandidates RankTypes(uint32 TypeBits, RHI::EMemoryType Type) const;
        VkResult    AllocateMemory(uint64 Size, uint32 TypeIndex, const void* Next, float Priority, VkDeviceMemory& Out) const;
        bool        FitsBudget(uint32 TypeIndex, uint64 Size) const;
        bool        AllocateShared(const VkMemoryRequirements& Requirements, float Priority, FMemoryAllocation& Out);
        bool        AllocateOwn(const VkMemoryRequirements& Requirements, RHI::EMemoryType Type, const void* Next, float Priority, bool bCheckBudget, FMemoryAllocation& Out);
        uint32      HeapOf(uint32 TypeIndex) const { return Properties.memoryTypes[TypeIndex].heapIndex; }
        // Caller holds Mutex.
        void        NoteBlockLocked(uint32 TypeIndex, uint64 Bytes, bool bAdded);
        void        NoteAllocationLocked(uint32 TypeIndex, uint64 Bytes, bool bAdded);

        const FApi*                      Api            = nullptr;
        VkPhysicalDevice                 PhysicalDevice = VK_NULL_HANDLE;
        VkDevice                         Device         = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties Properties     = {};
        bool                             bMemoryBudget   = false;
        bool                             bMemoryPriority = false;
        uint64                           SharedBlockSize[VK_MAX_MEMORY_HEAPS] = {};

        mutable FMutex                          Mutex;
        TVector<TUniquePtr<FSharedMemoryBlock>> SharedBlocks;
        FHeapBudget                             Held[VK_MAX_MEMORY_HEAPS];
    };
}
