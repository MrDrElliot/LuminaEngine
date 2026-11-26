#include "RuntimePCH.h"
#include "RangeAllocator.h"

#include "Core/Assertions/Assert.h"
#include <bit>

namespace Lumina
{
    void FRangeAllocator::MapSize(uint64 Size, uint32& OutFirst, uint32& OutSecond)
    {
        if (Size < kSecondLevelCount)
        {
            OutFirst  = 0;
            OutSecond = (uint32)Size;
            return;
        }

        const uint32 Log = 63u - (uint32)std::countl_zero(Size);
        OutFirst  = Log - kSecondLevelBits + 1u;
        OutSecond = (uint32)(Size >> (Log - kSecondLevelBits)) ^ kSecondLevelCount;
    }

    void FRangeAllocator::MapSizeRoundedUp(uint64 Size, uint32& OutFirst, uint32& OutSecond)
    {
        if (Size >= kSecondLevelCount)
        {
            const uint32 Log = 63u - (uint32)std::countl_zero(Size);
            const uint64 Step = 1ull << (Log - kSecondLevelBits);
            Size = Size + Step - 1u < Size ? Size : Size + Step - 1u;
        }
        MapSize(Size, OutFirst, OutSecond);
    }

    void FRangeAllocator::Reset(uint64 InCapacity)
    {
        Nodes.Reset();
        SpareNodes.Reset();
        for (uint32& Head : FreeHeads)
        {
            Head = kNoNode;
        }
        for (uint32& Mask : SecondLevelMasks)
        {
            Mask = 0u;
        }
        FirstLevelMask  = 0;
        Capacity        = InCapacity;
        UsedBytes       = 0;
        AllocationCount = 0;

        if (InCapacity > 0u)
        {
            const uint32 Whole = NewNode(0u, InCapacity);
            InsertFree(Whole);
        }
    }

    uint32 FRangeAllocator::NewNode(uint64 Offset, uint64 Size)
    {
        uint32 Index;
        if (!SpareNodes.IsEmpty())
        {
            Index = SpareNodes.Pop();
        }
        else
        {
            Index = (uint32)Nodes.Num();
            Nodes.AddDefaulted();
        }
        Nodes[Index] = FNode{ .Offset = Offset, .Size = Size };
        return Index;
    }

    void FRangeAllocator::ReleaseNode(uint32 Node)
    {
        Nodes[Node] = FNode{};
        SpareNodes.Add(Node);
    }

    void FRangeAllocator::InsertFree(uint32 Node)
    {
        uint32 First, Second;
        MapSize(Nodes[Node].Size, First, Second);
        uint32& Head = FreeHeads[First * kSecondLevelCount + Second];

        FNode& N   = Nodes[Node];
        N.bFree    = true;
        N.PrevFree = kNoNode;
        N.NextFree = Head;
        if (Head != kNoNode)
        {
            Nodes[Head].PrevFree = Node;
        }
        Head = Node;

        SecondLevelMasks[First] |= 1u << Second;
        FirstLevelMask          |= 1ull << First;
    }

    void FRangeAllocator::RemoveFree(uint32 Node)
    {
        FNode& N = Nodes[Node];
        if (N.PrevFree != kNoNode)
        {
            Nodes[N.PrevFree].NextFree = N.NextFree;
        }
        else
        {
            uint32 First, Second;
            MapSize(N.Size, First, Second);
            FreeHeads[First * kSecondLevelCount + Second] = N.NextFree;
            if (N.NextFree == kNoNode)
            {
                SecondLevelMasks[First] &= ~(1u << Second);
                if (SecondLevelMasks[First] == 0u)
                {
                    FirstLevelMask &= ~(1ull << First);
                }
            }
        }
        if (N.NextFree != kNoNode)
        {
            Nodes[N.NextFree].PrevFree = N.PrevFree;
        }
        N.PrevFree = kNoNode;
        N.NextFree = kNoNode;
        N.bFree    = false;
    }

    uint32 FRangeAllocator::FindFreeNode(uint64 Size) const
    {
        uint32 First, Second;
        MapSizeRoundedUp(Size, First, Second);
        if (First >= kFirstLevelCount)
        {
            return kNoNode;
        }

        uint32 SecondMask = SecondLevelMasks[First] & (Second < 32u ? ~0u << Second : 0u);
        if (SecondMask == 0u)
        {
            const uint64 FirstMask = First + 1u < 64u ? FirstLevelMask & (~0ull << (First + 1u)) : 0ull;
            if (FirstMask == 0ull)
            {
                return kNoNode;
            }
            First      = (uint32)std::countr_zero(FirstMask);
            SecondMask = SecondLevelMasks[First];
        }
        return FreeHeads[First * kSecondLevelCount + (uint32)std::countr_zero(SecondMask)];
    }

    bool FRangeAllocator::Allocate(uint64 Size, uint64 Alignment, FHandle& OutHandle, uint64& OutOffset)
    {
        ASSERT(Alignment != 0u && (Alignment & (Alignment - 1u)) == 0u);
        Size = Size == 0u ? 1u : Size;

        // A bin of exactly Size is tried first, since most offsets already sit aligned and padding for the worst case would skip a fitting range.
        uint32 Node = FindFreeNode(Size);
        if (Node != kNoNode)
        {
            const FNode& N = Nodes[Node];
            const uint64 Aligned = (N.Offset + Alignment - 1u) & ~(Alignment - 1u);
            if (Aligned - N.Offset + Size > N.Size)
            {
                Node = kNoNode;
            }
        }
        if (Node == kNoNode && Alignment > 1u)
        {
            Node = FindFreeNode(Size + Alignment - 1u);
        }
        if (Node == kNoNode)
        {
            return false;
        }

        RemoveFree(Node);

        const uint64 Aligned = (Nodes[Node].Offset + Alignment - 1u) & ~(Alignment - 1u);
        const uint64 Padding = Aligned - Nodes[Node].Offset;
        if (Padding > 0u)
        {
            // The range before a free one is never free itself, so the padding stands alone.
            const uint32 Front = NewNode(Nodes[Node].Offset, Padding);
            Nodes[Front].PrevPhysical = Nodes[Node].PrevPhysical;
            Nodes[Front].NextPhysical = Node;
            if (Nodes[Front].PrevPhysical != kNoNode)
            {
                Nodes[Nodes[Front].PrevPhysical].NextPhysical = Front;
            }
            Nodes[Node].PrevPhysical = Front;
            Nodes[Node].Offset      += Padding;
            Nodes[Node].Size        -= Padding;
            InsertFree(Front);
        }

        if (Nodes[Node].Size > Size)
        {
            const uint32 Back = NewNode(Nodes[Node].Offset + Size, Nodes[Node].Size - Size);
            Nodes[Back].PrevPhysical = Node;
            Nodes[Back].NextPhysical = Nodes[Node].NextPhysical;
            if (Nodes[Back].NextPhysical != kNoNode)
            {
                Nodes[Nodes[Back].NextPhysical].PrevPhysical = Back;
            }
            Nodes[Node].NextPhysical = Back;
            Nodes[Node].Size         = Size;
            InsertFree(Back);
        }

        UsedBytes += Nodes[Node].Size;
        ++AllocationCount;
        OutHandle = Node;
        OutOffset = Nodes[Node].Offset;
        return true;
    }

    void FRangeAllocator::Free(FHandle Handle)
    {
        ASSERT(Handle < (uint32)Nodes.Num() && !Nodes[Handle].bFree && Nodes[Handle].Size != 0u);

        uint32 Node = Handle;
        UsedBytes -= Nodes[Node].Size;
        --AllocationCount;

        const uint32 Prev = Nodes[Node].PrevPhysical;
        if (Prev != kNoNode && Nodes[Prev].bFree)
        {
            RemoveFree(Prev);
            Nodes[Prev].Size        += Nodes[Node].Size;
            Nodes[Prev].NextPhysical = Nodes[Node].NextPhysical;
            if (Nodes[Prev].NextPhysical != kNoNode)
            {
                Nodes[Nodes[Prev].NextPhysical].PrevPhysical = Prev;
            }
            ReleaseNode(Node);
            Node = Prev;
        }

        const uint32 Next = Nodes[Node].NextPhysical;
        if (Next != kNoNode && Nodes[Next].bFree)
        {
            RemoveFree(Next);
            Nodes[Node].Size        += Nodes[Next].Size;
            Nodes[Node].NextPhysical = Nodes[Next].NextPhysical;
            if (Nodes[Node].NextPhysical != kNoNode)
            {
                Nodes[Nodes[Node].NextPhysical].PrevPhysical = Node;
            }
            ReleaseNode(Next);
        }

        InsertFree(Node);
    }
}
