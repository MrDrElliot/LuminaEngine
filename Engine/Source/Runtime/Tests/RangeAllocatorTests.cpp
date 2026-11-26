#include <gtest/gtest.h>
#include <random>
#include "Containers/Vector.h"
#include "Memory/Allocators/RangeAllocator.h"

using namespace Lumina;

namespace
{
    struct FLive
    {
        FRangeAllocator::FHandle Handle;
        uint64                   Offset;
        uint64                   Size;
    };

    bool Overlaps(const FLive& A, const FLive& B)
    {
        return A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
    }
}

TEST(RangeAllocator, FillsTheWholeSpanAndRefusesMore)
{
    FRangeAllocator Ranges(1024);
    FRangeAllocator::FHandle Handle;
    uint64 Offset;

    ASSERT_TRUE(Ranges.Allocate(1024, 1, Handle, Offset));
    EXPECT_EQ(Offset, 0u);
    EXPECT_EQ(Ranges.GetUsedBytes(), 1024u);

    FRangeAllocator::FHandle Extra;
    EXPECT_FALSE(Ranges.Allocate(1, 1, Extra, Offset));
}

TEST(RangeAllocator, FreedNeighborsMergeBackIntoOneSpan)
{
    FRangeAllocator Ranges(4096);
    FRangeAllocator::FHandle A, B, C;
    uint64 Offset;
    ASSERT_TRUE(Ranges.Allocate(1024, 256, A, Offset));
    ASSERT_TRUE(Ranges.Allocate(1024, 256, B, Offset));
    ASSERT_TRUE(Ranges.Allocate(2048, 256, C, Offset));

    Ranges.Free(B);
    Ranges.Free(A);
    Ranges.Free(C);
    EXPECT_TRUE(Ranges.IsEmpty());

    FRangeAllocator::FHandle Whole;
    ASSERT_TRUE(Ranges.Allocate(4096, 1, Whole, Offset));
    EXPECT_EQ(Offset, 0u);
}

TEST(RangeAllocator, HonorsLargeAlignmentsAfterAnUnalignedRange)
{
    FRangeAllocator Ranges(1u << 20);
    FRangeAllocator::FHandle Small, Aligned;
    uint64 SmallOffset, AlignedOffset;
    ASSERT_TRUE(Ranges.Allocate(100, 4, Small, SmallOffset));
    ASSERT_TRUE(Ranges.Allocate(4096, 65536, Aligned, AlignedOffset));
    EXPECT_EQ(AlignedOffset % 65536, 0u);
    EXPECT_GE(AlignedOffset, SmallOffset + 100);

    // The padding before the aligned range goes back to the pool.
    FRangeAllocator::FHandle Pad;
    uint64 PadOffset;
    ASSERT_TRUE(Ranges.Allocate(1000, 4, Pad, PadOffset));
    EXPECT_LT(PadOffset, AlignedOffset);
}

TEST(RangeAllocator, ExactFitIsFoundWhenOnlyItRemains)
{
    FRangeAllocator Ranges(3 * 65536);
    FRangeAllocator::FHandle A, B, C;
    uint64 Offset;
    ASSERT_TRUE(Ranges.Allocate(65536, 65536, A, Offset));
    ASSERT_TRUE(Ranges.Allocate(65536, 65536, B, Offset));
    ASSERT_TRUE(Ranges.Allocate(65536, 65536, C, Offset));
    Ranges.Free(B);

    FRangeAllocator::FHandle Again;
    ASSERT_TRUE(Ranges.Allocate(65536, 65536, Again, Offset));
    EXPECT_EQ(Offset, 65536u);
}

TEST(RangeAllocator, RandomChurnNeverOverlapsAndEndsEmpty)
{
    constexpr uint64 kCapacity = 64ull << 20;
    FRangeAllocator Ranges(kCapacity);
    std::mt19937_64 Random(1234);
    TVector<FLive> Live;
    uint64 Expected = 0;

    for (int32 Step = 0; Step < 20000; ++Step)
    {
        const bool bAllocate = Live.IsEmpty() || (Random() % 3u) != 0u;
        if (bAllocate)
        {
            const uint64 Size      = 1u + Random() % (256u * 1024u);
            const uint64 Alignment = 1ull << (Random() % 17u);
            FLive Entry{};
            if (!Ranges.Allocate(Size, Alignment, Entry.Handle, Entry.Offset))
            {
                continue;
            }
            Entry.Size = Size;
            ASSERT_EQ(Entry.Offset % Alignment, 0u);
            ASSERT_LE(Entry.Offset + Size, kCapacity);
            for (const FLive& Other : Live)
            {
                ASSERT_FALSE(Overlaps(Entry, Other));
            }
            Live.Add(Entry);
            Expected += Size;
        }
        else
        {
            const int32 Index = (int32)(Random() % (uint64)Live.Num());
            Ranges.Free(Live[Index].Handle);
            Expected -= Live[Index].Size;
            Live.RemoveAtSwap(Index);
        }
        ASSERT_EQ(Ranges.GetUsedBytes(), Expected);
    }

    for (const FLive& Entry : Live)
    {
        Ranges.Free(Entry.Handle);
    }
    EXPECT_TRUE(Ranges.IsEmpty());

    FRangeAllocator::FHandle Whole;
    uint64 Offset;
    EXPECT_TRUE(Ranges.Allocate(kCapacity, 1, Whole, Offset));
}
