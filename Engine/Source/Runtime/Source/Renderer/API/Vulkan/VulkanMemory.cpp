#include "RuntimePCH.h"
#include "VulkanMemory.h"

#include "VulkanAllocator.h"
#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include <bit>

namespace Lumina::Vulkan
{
    struct FSharedMemoryBlock
    {
        VkDeviceMemory  Memory    = VK_NULL_HANDLE;
        uint64          Size      = 0;
        uint32          TypeIndex = 0;
        FRangeAllocator Ranges;
    };

    namespace
    {
        // Large enough that a streamed level lands in a few blocks, small enough that a half-empty one wastes little.
        constexpr uint64 kSharedBlockSize = 256ull << 20;
        // A heap this small (an integrated GPU's carve-out) splits into eighths instead.
        constexpr uint64 kSmallHeapSize   = 2ull << 30;
        constexpr float  kDefaultPriority    = 0.5f;
        constexpr float  kAttachmentPriority = 1.0f;
        // Without the budget extension, the share of a heap the process assumes it may fill.
        constexpr float  kAssumedBudgetShare = 0.8f;

        // Types no resource of the RHI may land in, since they need features it never enables.
        constexpr VkMemoryPropertyFlags kExcludedProperties = VK_MEMORY_PROPERTY_PROTECTED_BIT
            | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;

        struct FTypePreference
        {
            VkMemoryPropertyFlags Required;
            VkMemoryPropertyFlags Preferred;
            VkMemoryPropertyFlags Avoided;
        };

        FTypePreference PreferenceFor(RHI::EMemoryType Type)
        {
            switch (Type)
            {
            // Device local first, which is the resizable BAR when the system has one, and cached host memory last since writes never read back.
            case RHI::EMemoryType::CPUWrite:
                return { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT };
            case RHI::EMemoryType::CPURead:
                return { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT };
            case RHI::EMemoryType::GPUOnly:
            default:
                return { 0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT };
            }
        }

        bool IsOutOfMemory(VkResult Result)
        {
            return Result == VK_ERROR_OUT_OF_DEVICE_MEMORY || Result == VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    FDeviceMemoryAllocator::FDeviceMemoryAllocator() = default;
    FDeviceMemoryAllocator::~FDeviceMemoryAllocator() = default;

    void FDeviceMemoryAllocator::Initialize(const FApi& InApi, VkPhysicalDevice InPhysicalDevice, VkDevice InDevice, bool bInMemoryBudget, bool bInMemoryPriority)
    {
        Api             = &InApi;
        PhysicalDevice  = InPhysicalDevice;
        Device          = InDevice;
        bMemoryBudget   = bInMemoryBudget;
        bMemoryPriority = bInMemoryPriority;
        Api->GetPhysicalDeviceMemoryProperties(PhysicalDevice, &Properties);

        for (uint32 Heap = 0; Heap < Properties.memoryHeapCount; ++Heap)
        {
            const uint64 HeapSize = Properties.memoryHeaps[Heap].size;
            SharedBlockSize[Heap] = HeapSize > kSmallHeapSize ? kSharedBlockSize : std::bit_ceil(Math::Max<uint64>(HeapSize / 8u, 16ull << 20));
            Held[Heap] = {};
        }
    }

    void FDeviceMemoryAllocator::Shutdown()
    {
        FScopeLock Lock(Mutex);
        for (const TUniquePtr<FSharedMemoryBlock>& Block : SharedBlocks)
        {
            if (!Block->Ranges.IsEmpty())
            {
                LOG_WARN("RHI: an image memory block still held {} images ({} KiB) at shutdown.",
                         Block->Ranges.GetAllocationCount(), Block->Ranges.GetUsedBytes() / 1024u);
            }
            Api->FreeMemory(Device, Block->Memory, HostAllocator());
        }
        SharedBlocks.Reset();
    }

    FDeviceMemoryAllocator::FCandidates FDeviceMemoryAllocator::RankTypes(uint32 TypeBits, RHI::EMemoryType Type) const
    {
        const FTypePreference Preference = PreferenceFor(Type);
        uint32 Costs[VK_MAX_MEMORY_TYPES];

        FCandidates Out;
        for (uint32 TypeIndex = 0; TypeIndex < Properties.memoryTypeCount; ++TypeIndex)
        {
            const VkMemoryPropertyFlags Flags = Properties.memoryTypes[TypeIndex].propertyFlags;
            if ((TypeBits & (1u << TypeIndex)) == 0u || (Flags & Preference.Required) != Preference.Required || (Flags & kExcludedProperties) != 0u)
            {
                continue;
            }

            const uint32 Cost = (uint32)std::popcount(Preference.Preferred & ~Flags) + (uint32)std::popcount(Preference.Avoided & Flags);
            uint32 Slot = Out.Num++;
            while (Slot > 0u && Costs[Slot - 1u] > Cost)
            {
                Costs[Slot]     = Costs[Slot - 1u];
                Out.Types[Slot] = Out.Types[Slot - 1u];
                --Slot;
            }
            Costs[Slot]     = Cost;
            Out.Types[Slot] = TypeIndex;
        }
        return Out;
    }

    VkResult FDeviceMemoryAllocator::AllocateMemory(uint64 Size, uint32 TypeIndex, const void* Next, float Priority, VkDeviceMemory& Out) const
    {
        const VkMemoryPriorityAllocateInfoEXT PriorityInfo
        {
            .sType    = VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT,
            .pNext    = Next,
            .priority = Priority,
        };
        const VkMemoryAllocateInfo Info
        {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext           = bMemoryPriority ? &PriorityInfo : Next,
            .allocationSize  = Size,
            .memoryTypeIndex = TypeIndex,
        };
        return Api->AllocateMemory(Device, &Info, HostAllocator(), &Out);
    }

    bool FDeviceMemoryAllocator::FitsBudget(uint32 TypeIndex, uint64 Size) const
    {
        FHeapBudget Budgets[VK_MAX_MEMORY_HEAPS];
        GetHeapBudgets(Budgets);
        const FHeapBudget& Heap = Budgets[HeapOf(TypeIndex)];
        return Heap.UsageBytes + Size <= Heap.BudgetBytes;
    }

    void FDeviceMemoryAllocator::NoteBlockLocked(uint32 TypeIndex, uint64 Bytes, bool bAdded)
    {
        FHeapBudget& Heap = Held[HeapOf(TypeIndex)];
        Heap.BlockBytes = bAdded ? Heap.BlockBytes + Bytes : Heap.BlockBytes - Bytes;
        Heap.BlockCount = bAdded ? Heap.BlockCount + 1u : Heap.BlockCount - 1u;
    }

    void FDeviceMemoryAllocator::NoteAllocationLocked(uint32 TypeIndex, uint64 Bytes, bool bAdded)
    {
        FHeapBudget& Heap = Held[HeapOf(TypeIndex)];
        Heap.AllocationBytes = bAdded ? Heap.AllocationBytes + Bytes : Heap.AllocationBytes - Bytes;
        Heap.AllocationCount = bAdded ? Heap.AllocationCount + 1u : Heap.AllocationCount - 1u;
    }

    bool FDeviceMemoryAllocator::AllocateOwn(const VkMemoryRequirements& Requirements, RHI::EMemoryType Type, const void* Next, float Priority,
                                             bool bCheckBudget, FMemoryAllocation& Out)
    {
        const FCandidates Candidates = RankTypes(Requirements.memoryTypeBits, Type);

        // Over budget a type is tried only after every type within it, so a full resizable BAR spills to host memory rather than failing.
        for (uint32 Pass = 0; Pass < (bCheckBudget ? 2u : 1u); ++Pass)
        {
            for (uint32 Index = 0; Index < Candidates.Num; ++Index)
            {
                const uint32 TypeIndex = Candidates.Types[Index];
                if (bCheckBudget && (Pass == 0u) != FitsBudget(TypeIndex, Requirements.size))
                {
                    continue;
                }

                VkDeviceMemory Memory = VK_NULL_HANDLE;
                const VkResult Result = AllocateMemory(Requirements.size, TypeIndex, Next, Priority, Memory);
                if (IsOutOfMemory(Result))
                {
                    continue;
                }
                if (Result != VK_SUCCESS)
                {
                    return false;
                }

                void* Mapped = nullptr;
                if ((Properties.memoryTypes[TypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u
                    && Api->MapMemory(Device, Memory, 0, VK_WHOLE_SIZE, 0, &Mapped) != VK_SUCCESS)
                {
                    Api->FreeMemory(Device, Memory, HostAllocator());
                    return false;
                }

                Out = FMemoryAllocation{ .Memory = Memory, .Offset = 0, .Size = Requirements.size, .Mapped = Mapped, .TypeIndex = TypeIndex };

                FScopeLock Lock(Mutex);
                NoteBlockLocked(TypeIndex, Requirements.size, true);
                NoteAllocationLocked(TypeIndex, Requirements.size, true);
                return true;
            }
        }
        return false;
    }

    bool FDeviceMemoryAllocator::AllocateShared(const VkMemoryRequirements& Requirements, float Priority, FMemoryAllocation& Out)
    {
        const FCandidates Candidates = RankTypes(Requirements.memoryTypeBits, RHI::EMemoryType::GPUOnly);

        FScopeLock Lock(Mutex);
        for (uint32 Index = 0; Index < Candidates.Num; ++Index)
        {
            const uint32 TypeIndex = Candidates.Types[Index];
            FSharedMemoryBlock* Chosen = nullptr;
            uint64 Offset = 0;
            FRangeAllocator::FHandle Range = FRangeAllocator::kInvalidHandle;

            for (const TUniquePtr<FSharedMemoryBlock>& Block : SharedBlocks)
            {
                if (Block->TypeIndex == TypeIndex && Block->Ranges.Allocate(Requirements.size, Requirements.alignment, Range, Offset))
                {
                    Chosen = Block.Get();
                    break;
                }
            }

            if (Chosen == nullptr)
            {
                const uint64 BlockSize = SharedBlockSize[HeapOf(TypeIndex)];
                VkDeviceMemory Memory = VK_NULL_HANDLE;
                if (Requirements.size > BlockSize || AllocateMemory(BlockSize, TypeIndex, nullptr, Priority, Memory) != VK_SUCCESS)
                {
                    continue;
                }

                TUniquePtr<FSharedMemoryBlock> Block = MakeUnique<FSharedMemoryBlock>();
                Block->Memory    = Memory;
                Block->Size      = BlockSize;
                Block->TypeIndex = TypeIndex;
                Block->Ranges.Reset(BlockSize);
                NoteBlockLocked(TypeIndex, BlockSize, true);
                LOG_INFO("RHI: new {} MiB image memory block in memory type {}.", BlockSize >> 20, TypeIndex);

                Chosen = Block.Get();
                SharedBlocks.Add(Move(Block));
                if (!Chosen->Ranges.Allocate(Requirements.size, Requirements.alignment, Range, Offset))
                {
                    continue;
                }
            }

            Out = FMemoryAllocation
            {
                .Memory    = Chosen->Memory,
                .Offset    = Offset,
                .Size      = Requirements.size,
                .Mapped    = nullptr,
                .TypeIndex = TypeIndex,
                .Block     = Chosen,
                .Range     = Range,
            };
            NoteAllocationLocked(TypeIndex, Requirements.size, true);
            return true;
        }
        return false;
    }

    bool FDeviceMemoryAllocator::AllocateBuffer(VkBuffer Buffer, RHI::EMemoryType Type, FMemoryAllocation& Out)
    {
        VkMemoryRequirements Requirements = {};
        Api->GetBufferMemoryRequirements(Device, Buffer, &Requirements);

        const VkMemoryDedicatedAllocateInfo Dedicated
        {
            .sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext  = nullptr,
            .image  = VK_NULL_HANDLE,
            .buffer = Buffer,
        };
        const VkMemoryAllocateFlagsInfo Flags
        {
            .sType      = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .pNext      = &Dedicated,
            .flags      = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
            .deviceMask = 0,
        };

        if (!AllocateOwn(Requirements, Type, &Flags, kDefaultPriority, Type == RHI::EMemoryType::CPUWrite, Out))
        {
            return false;
        }
        if (Api->BindBufferMemory(Device, Buffer, Out.Memory, 0) != VK_SUCCESS)
        {
            Free(Out);
            return false;
        }
        return true;
    }

    bool FDeviceMemoryAllocator::AllocateImage(VkImage Image, bool bAttachment, FMemoryAllocation& Out)
    {
        VkMemoryDedicatedRequirements DedicatedRequirements{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
        VkMemoryRequirements2 Requirements{ .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &DedicatedRequirements };
        const VkImageMemoryRequirementsInfo2 Info{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = Image };
        Api->GetImageMemoryRequirements2(Device, &Info, &Requirements);

        const VkMemoryRequirements& Needs = Requirements.memoryRequirements;
        const FCandidates Candidates = RankTypes(Needs.memoryTypeBits, RHI::EMemoryType::GPUOnly);
        if (Candidates.Num == 0u)
        {
            return false;
        }
        const float  Priority = bAttachment ? kAttachmentPriority : kDefaultPriority;
        const uint32 Heap     = HeapOf(Candidates.Types[0]);

        // Half a block or more would strand the rest of one, and the driver's preference usually marks a render target it can compress.
        const bool bOwn = DedicatedRequirements.requiresDedicatedAllocation || DedicatedRequirements.prefersDedicatedAllocation
                       || Needs.size * 2u > SharedBlockSize[Heap];

        bool bAllocated = !bOwn && AllocateShared(Needs, Priority, Out);
        if (!bAllocated)
        {
            const VkMemoryDedicatedAllocateInfo Dedicated
            {
                .sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .pNext  = nullptr,
                .image  = Image,
                .buffer = VK_NULL_HANDLE,
            };
            bAllocated = AllocateOwn(Needs, RHI::EMemoryType::GPUOnly, &Dedicated, Priority, false, Out);
        }
        if (!bAllocated)
        {
            return false;
        }

        if (Api->BindImageMemory(Device, Image, Out.Memory, Out.Offset) != VK_SUCCESS)
        {
            Free(Out);
            return false;
        }
        return true;
    }

    void FDeviceMemoryAllocator::Free(FMemoryAllocation& Allocation)
    {
        if (!Allocation.IsValid())
        {
            return;
        }

        VkDeviceMemory Release = VK_NULL_HANDLE;
        {
            FScopeLock Lock(Mutex);
            NoteAllocationLocked(Allocation.TypeIndex, Allocation.Size, false);

            if (FSharedMemoryBlock* Block = Allocation.Block)
            {
                Block->Ranges.Free(Allocation.Range);

                // One empty block per type stays, so streaming churn around a single block never reaches the driver.
                const bool bSpareExists = Block->Ranges.IsEmpty() && Algo::AnyOf(SharedBlocks, [Block](const TUniquePtr<FSharedMemoryBlock>& Other)
                {
                    return Other.Get() != Block && Other->TypeIndex == Block->TypeIndex && Other->Ranges.IsEmpty();
                });
                if (bSpareExists)
                {
                    Release = Block->Memory;
                    NoteBlockLocked(Block->TypeIndex, Block->Size, false);
                    SharedBlocks.RemoveAll([Block](const TUniquePtr<FSharedMemoryBlock>& Other) { return Other.Get() == Block; });
                }
            }
            else
            {
                Release = Allocation.Memory;
                NoteBlockLocked(Allocation.TypeIndex, Allocation.Size, false);
            }
        }

        if (Release != VK_NULL_HANDLE)
        {
            Api->FreeMemory(Device, Release, HostAllocator());
        }
        Allocation = FMemoryAllocation{};
    }

    void FDeviceMemoryAllocator::GetHeapBudgets(FHeapBudget (&Out)[VK_MAX_MEMORY_HEAPS]) const
    {
        {
            FScopeLock Lock(Mutex);
            for (uint32 Heap = 0; Heap < VK_MAX_MEMORY_HEAPS; ++Heap)
            {
                Out[Heap] = Held[Heap];
            }
        }

        VkPhysicalDeviceMemoryBudgetPropertiesEXT Budget{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
        if (bMemoryBudget)
        {
            VkPhysicalDeviceMemoryProperties2 Properties2{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &Budget };
            Api->GetPhysicalDeviceMemoryProperties2(PhysicalDevice, &Properties2);
        }

        for (uint32 Heap = 0; Heap < Properties.memoryHeapCount; ++Heap)
        {
            Out[Heap].BudgetBytes = bMemoryBudget ? Budget.heapBudget[Heap] : (uint64)((float)Properties.memoryHeaps[Heap].size * kAssumedBudgetShare);
            Out[Heap].UsageBytes  = bMemoryBudget ? Budget.heapUsage[Heap] : Out[Heap].BlockBytes;
        }
    }
}
