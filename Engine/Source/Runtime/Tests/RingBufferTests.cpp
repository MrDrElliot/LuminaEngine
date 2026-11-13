#include <gtest/gtest.h>

#include "Containers/RingBuffer.h"

using namespace Lumina;

TEST(RingBuffer, VersionMovesOnEveryAdd)
{
    TRingBuffer<int> Buffer(4);
    const uint64 StartVersion = Buffer.GetVersion();

    Buffer.Add(1);
    EXPECT_NE(Buffer.GetVersion(), StartVersion);

    const uint64 AfterFirstAdd = Buffer.GetVersion();
    Buffer.Add(2);
    EXPECT_NE(Buffer.GetVersion(), AfterFirstAdd);
}

TEST(RingBuffer, VersionKeepsMovingOnceFull)
{
    TRingBuffer<int> Buffer(4);
    for (int i = 0; i < 4; ++i)
    {
        Buffer.Add(i);
    }
    ASSERT_TRUE(Buffer.IsFull());

    const uint64 VersionWhenFull = Buffer.GetVersion();
    const int32 NumWhenFull = Buffer.Num();

    // A cache keyed on Num() alone would miss this, since Num() stays pinned at capacity.
    Buffer.Add(4);

    EXPECT_EQ(Buffer.Num(), NumWhenFull);
    EXPECT_NE(Buffer.GetVersion(), VersionWhenFull);
    EXPECT_EQ(Buffer.Last(), 4);
}

TEST(RingBuffer, ResetChangesTheVersionEvenIfRefilledToTheSameCount)
{
    TRingBuffer<int> Buffer(4);
    Buffer.Add(1);
    Buffer.Add(2);
    const int32 NumBeforeReset = Buffer.Num();
    const uint64 VersionBeforeReset = Buffer.GetVersion();

    Buffer.Reset();
    Buffer.Add(3);
    Buffer.Add(4);

    EXPECT_EQ(Buffer.Num(), NumBeforeReset);
    EXPECT_NE(Buffer.GetVersion(), VersionBeforeReset);
}

TEST(RingBuffer, SetCapacityChangesTheVersion)
{
    TRingBuffer<int> Buffer(4);
    Buffer.Add(1);
    const uint64 VersionBeforeResize = Buffer.GetVersion();

    Buffer.SetCapacity(8);

    EXPECT_NE(Buffer.GetVersion(), VersionBeforeResize);
}
