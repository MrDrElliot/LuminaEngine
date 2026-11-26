#pragma once

#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Hands out aligned ranges of one span in constant time with two-level segregated fit, merging a freed range with its free neighbors at once.
    class RUNTIME_API FRangeAllocator
    {
    public:

        using FHandle = uint32;
        static constexpr FHandle kInvalidHandle = ~0u;

        FRangeAllocator() = default;
        explicit FRangeAllocator(uint64 InCapacity) { Reset(InCapacity); }

        // Forgets every range and starts over with one free span of InCapacity.
        void Reset(uint64 InCapacity);

        // Alignment is a power of two, and false means no free range holds Size at it.
        bool Allocate(uint64 Size, uint64 Alignment, FHandle& OutHandle, uint64& OutOffset);

        void Free(FHandle Handle);

        uint64 GetCapacity() const { return Capacity; }
        uint64 GetUsedBytes() const { return UsedBytes; }
        uint32 GetAllocationCount() const { return AllocationCount; }
        bool   IsEmpty() const { return AllocationCount == 0u; }

    private:

        static constexpr uint32 kSecondLevelBits  = 5;
        static constexpr uint32 kSecondLevelCount = 1u << kSecondLevelBits;
        static constexpr uint32 kFirstLevelCount  = 64 - kSecondLevelBits + 1;
        static constexpr uint32 kNoNode           = ~0u;

        // Neighbors in address order and in the free list of the node's bin, kNoNode at either end.
        struct FNode
        {
            uint64 Offset       = 0;
            uint64 Size         = 0;
            uint32 PrevPhysical = kNoNode;
            uint32 NextPhysical = kNoNode;
            uint32 PrevFree     = kNoNode;
            uint32 NextFree     = kNoNode;
            bool   bFree        = false;
        };

        static void MapSize(uint64 Size, uint32& OutFirst, uint32& OutSecond);
        // The bin whose every range is at least Size, so its head always fits.
        static void MapSizeRoundedUp(uint64 Size, uint32& OutFirst, uint32& OutSecond);

        uint32 FindFreeNode(uint64 Size) const;
        void   InsertFree(uint32 Node);
        void   RemoveFree(uint32 Node);
        uint32 NewNode(uint64 Offset, uint64 Size);
        void   ReleaseNode(uint32 Node);

        TVector<FNode>  Nodes;
        TVector<uint32> SpareNodes;
        uint32          FreeHeads[kFirstLevelCount * kSecondLevelCount] = {};
        uint32          SecondLevelMasks[kFirstLevelCount] = {};
        uint64          FirstLevelMask  = 0;
        uint64          Capacity        = 0;
        uint64          UsedBytes       = 0;
        uint32          AllocationCount = 0;
    };
}
