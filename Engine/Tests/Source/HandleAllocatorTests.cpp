#include <gtest/gtest.h>

#include "Containers/HandleAllocator.h"

using namespace Lumina;

namespace
{
    TVector<uint32> CollectAllocated(const FHandleAllocator& A)
    {
        TVector<uint32> Out;
        A.ForEachAllocated([&](uint32 Slot) { Out.push_back(Slot); });
        return Out;
    }
}

TEST(HandleAllocator, HandsOutLowestFreeSlot)
{
    FHandleAllocator A(256);
    for (uint32 i = 0; i < 8; ++i)
    {
        EXPECT_EQ(A.Alloc(), i);
    }
    EXPECT_EQ(A.GetNumAllocated(), 8u);
}

// The packing property the whole design exists for: a hole anywhere must be refilled before the
// occupied range grows, or the region creeps upward and every sweep over it gets longer.
TEST(HandleAllocator, RefillsHolesBeforeExtending)
{
    FHandleAllocator A(256);
    for (uint32 i = 0; i < 100; ++i)
    {
        A.Alloc();
    }

    A.Free(70);
    A.Free(3);
    A.Free(42);

    EXPECT_EQ(A.Alloc(), 3u);
    EXPECT_EQ(A.Alloc(), 42u);
    EXPECT_EQ(A.Alloc(), 70u);
    EXPECT_EQ(A.Alloc(), 100u);   // only now does it extend
}

// The search hint is what makes allocation cheap; a free below it must pull it back or that slot is
// invisible until the region wraps. This is the bug the hint invites.
TEST(HandleAllocator, FreeBelowTheHintIsStillFound)
{
    FHandleAllocator A(4096);
    for (uint32 i = 0; i < 3000; ++i)
    {
        A.Alloc();
    }

    A.Free(5);                    // word 0, far below where the hint now sits
    EXPECT_EQ(A.Alloc(), 5u);
}

TEST(HandleAllocator, ExhaustionReturnsInvalidAndDoesNotWrap)
{
    FHandleAllocator A(64);
    for (uint32 i = 0; i < 64; ++i)
    {
        EXPECT_NE(A.Alloc(), FHandleAllocator::kInvalidHandle);
    }

    EXPECT_EQ(A.Alloc(), FHandleAllocator::kInvalidHandle);
    EXPECT_EQ(A.Alloc(), FHandleAllocator::kInvalidHandle);   // still full, still no wrap
    EXPECT_EQ(A.GetNumAllocated(), 64u);

    A.Free(31);
    EXPECT_EQ(A.Alloc(), 31u);
}

// A capacity that is not a multiple of 64 leaves padding bits in the last word. They are pre-marked
// allocated so the scan cannot hand them out -- if that broke, this returns a slot past the end and the
// caller writes a descriptor outside the heap.
TEST(HandleAllocator, NeverHandsOutPaddingPastCapacity)
{
    FHandleAllocator A(100);

    uint32 Count = 0;
    for (;;)
    {
        const uint32 Slot = A.Alloc();
        if (Slot == FHandleAllocator::kInvalidHandle)
        {
            break;
        }
        EXPECT_LT(Slot, 100u);
        ++Count;
    }

    EXPECT_EQ(Count, 100u);
    EXPECT_EQ(CollectAllocated(A).size(), 100u);
}

TEST(HandleAllocator, MarkAllocatedClaimsAPublishedSlot)
{
    FHandleAllocator A(256);

    EXPECT_TRUE(A.MarkAllocated(200));
    EXPECT_TRUE(A.IsAllocated(200));
    EXPECT_FALSE(A.MarkAllocated(200));          // already taken -- the repoint case
    EXPECT_FALSE(A.MarkAllocated(999));          // out of range

    EXPECT_NE(A.Alloc(), 200u);                  // never handed out twice
    EXPECT_EQ(A.GetNumAllocated(), 2u);
}

TEST(HandleAllocator, DoubleFreeIsANoOpNotACorruption)
{
    FHandleAllocator A(256);
    const uint32 Slot = A.Alloc();

    A.Free(Slot);
    A.Free(Slot);                                // must not double-decrement the count
    A.Free(9999);                                // out of range

    EXPECT_EQ(A.GetNumAllocated(), 0u);
    EXPECT_EQ(A.Alloc(), Slot);
}

TEST(HandleAllocator, ForEachAllocatedVisitsExactlyTheLiveSlotsInOrder)
{
    FHandleAllocator A(300);
    for (uint32 i = 0; i < 300; ++i)
    {
        A.Alloc();
    }
    for (uint32 i = 0; i < 300; i += 3)
    {
        A.Free(i);
    }

    const TVector<uint32> Live = CollectAllocated(A);
    ASSERT_EQ(Live.size(), A.GetNumAllocated());

    for (size_t i = 1; i < Live.size(); ++i)
    {
        EXPECT_LT(Live[i - 1], Live[i]);         // ascending
    }
    for (uint32 Slot : Live)
    {
        EXPECT_NE(Slot % 3u, 0u);
        EXPECT_TRUE(A.IsAllocated(Slot));
    }
}

TEST(HandleAllocator, HighWaterMarkIsMonotonic)
{
    FHandleAllocator A(256);
    for (uint32 i = 0; i < 40; ++i)
    {
        A.Alloc();
    }
    EXPECT_EQ(A.GetHighWaterMark(), 40u);

    for (uint32 i = 0; i < 40; ++i)
    {
        A.Free(i);
    }
    EXPECT_EQ(A.GetNumAllocated(), 0u);
    EXPECT_EQ(A.GetHighWaterMark(), 40u);        // an upper bound, deliberately not a live max
}

TEST(HandleAllocator, ResetReclaimsEverything)
{
    FHandleAllocator A(128);
    for (uint32 i = 0; i < 128; ++i)
    {
        A.Alloc();
    }
    EXPECT_EQ(A.Alloc(), FHandleAllocator::kInvalidHandle);

    A.Reset(128);
    EXPECT_EQ(A.GetNumAllocated(), 0u);
    EXPECT_EQ(A.GetHighWaterMark(), 0u);
    EXPECT_EQ(A.Alloc(), 0u);
}

// Mirrors the bindless heap's actual usage: churn a mostly-full region and confirm every live slot is
// unique and in range. A duplicate here is two textures sharing one descriptor.
TEST(HandleAllocator, ChurnKeepsSlotsUniqueAndInRange)
{
    FHandleAllocator A(1024);
    TVector<uint32>  Held;

    for (uint32 i = 0; i < 900; ++i)
    {
        Held.push_back(A.Alloc());
    }

    for (uint32 Round = 0; Round < 50; ++Round)
    {
        for (size_t i = Round % 7; i < Held.size(); i += 7)
        {
            A.Free(Held[i]);
            Held[i] = FHandleAllocator::kInvalidHandle;
        }
        for (size_t i = 0; i < Held.size(); ++i)
        {
            if (Held[i] == FHandleAllocator::kInvalidHandle)
            {
                Held[i] = A.Alloc();
                ASSERT_NE(Held[i], FHandleAllocator::kInvalidHandle);
            }
        }
    }

    TVector<bool> Seen(1024, false);
    for (uint32 Slot : Held)
    {
        ASSERT_LT(Slot, 1024u);
        EXPECT_FALSE(Seen[Slot]);
        Seen[Slot] = true;
    }
    EXPECT_EQ(A.GetNumAllocated(), (uint32)Held.size());
}
