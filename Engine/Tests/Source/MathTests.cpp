#include <gtest/gtest.h>
#include "Core/Math/Math.h"

using namespace Lumina::Math;

TEST(MathTests, NextPowerOfTwo_Basic)
{
    EXPECT_EQ(NextPowerOfTwo(1), 1);
    EXPECT_EQ(NextPowerOfTwo(2), 2);
    EXPECT_EQ(NextPowerOfTwo(3), 4);
    EXPECT_EQ(NextPowerOfTwo(4), 4);
    EXPECT_EQ(NextPowerOfTwo(5), 8);
    EXPECT_EQ(NextPowerOfTwo(16), 16);
    EXPECT_EQ(NextPowerOfTwo(17), 32);
}

TEST(MathTests, AlignUp_PowerOfTwoAlignment)
{
    EXPECT_EQ(AlignUp<uint64>(0, 16), 0);
    EXPECT_EQ(AlignUp<uint64>(1, 16), 16);
    EXPECT_EQ(AlignUp<uint64>(15, 16), 16);
    EXPECT_EQ(AlignUp<uint64>(16, 16), 16);
    EXPECT_EQ(AlignUp<uint64>(17, 16), 32);
    EXPECT_EQ(AlignUp<uint64>(31, 16), 32);
}

TEST(MathTests, Max_Min)
{
    EXPECT_EQ(Max(1, 2), 2);
    EXPECT_EQ(Max(5, 2), 5);
    EXPECT_EQ(Min(1, 2), 1);
    EXPECT_EQ(Min(5, 2), 2);
}

TEST(MathTests, Clamp_Basic)
{
    EXPECT_EQ(Clamp(5, 1, 10), 5);
    EXPECT_EQ(Clamp(0, 1, 10), 1);
    EXPECT_EQ(Clamp(20, 1, 10), 10);
}

TEST(MathTests, Abs_Basic)
{
    EXPECT_EQ(Abs(-5), 5);
    EXPECT_EQ(Abs(5), 5);
    EXPECT_EQ(Abs(0), 0);
}

TEST(MathTests, Floor_Float)
{
    EXPECT_FLOAT_EQ(Floor(1.9f), 1.0f);
    EXPECT_FLOAT_EQ(Floor(-1.1f), -2.0f);
    EXPECT_FLOAT_EQ(Floor(2.0f), 2.0f);
}

TEST(MathTests, Pow_Basic)
{
    EXPECT_FLOAT_EQ(Pow(2.0f, 3.0f), 8.0f);
    EXPECT_FLOAT_EQ(Pow(4.0f, 0.5f), 2.0f);
}

TEST(MathTests, Lerp_Basic)
{
    EXPECT_FLOAT_EQ(Lerp(0.0f, 10.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(Lerp(0.0f, 10.0f, 1.0f), 10.0f);
    EXPECT_FLOAT_EQ(Lerp(0.0f, 10.0f, 0.5f), 5.0f);
}

TEST(MathTests, IsNearlyEqual_Basic)
{
    EXPECT_TRUE(IsNearlyEqual(1.0f, 1.0f, 0.0001f));
    EXPECT_TRUE(IsNearlyEqual(1.0f, 1.00001f, 0.001f));
    EXPECT_FALSE(IsNearlyEqual(1.0f, 1.1f, 0.001f));
}

TEST(MathTests, CountTrailingZeros64_Basic)
{
    EXPECT_EQ(CountTrailingZeros64(0), 64);
    EXPECT_EQ(CountTrailingZeros64(1), 0);
    EXPECT_EQ(CountTrailingZeros64(2), 1);
    EXPECT_EQ(CountTrailingZeros64(4), 2);
    EXPECT_EQ(CountTrailingZeros64(8), 3);
    EXPECT_EQ(CountTrailingZeros64(16), 4);
}

TEST(MathTests, RandRange_IsInBounds)
{
    for (int i = 0; i < 1000; ++i)
    {
        uint32 v = RandRange<uint32>(10, 20);
        EXPECT_GE(v, 10u);
        EXPECT_LE(v, 20u);
    }
}

TEST(MathTests, RandRange_SwapsBounds)
{
    for (int i = 0; i < 100; ++i)
    {
        uint32 v = RandRange<uint32>(20, 10);
        EXPECT_GE(v, 10u);
        EXPECT_LE(v, 20u);
    }
}

TEST(MathTests, RandRange_FloatIsInBounds)
{
    for (int i = 0; i < 1000; ++i)
    {
        const float v = RandRange(-2.5f, 7.5f);
        EXPECT_GE(v, -2.5f);
        EXPECT_LE(v, 7.5f);
    }
}

TEST(MathTests, RandomStream_SameSeedSameSequence)
{
    Lumina::FRandomStream A(12345u);
    Lumina::FRandomStream B(12345u);

    for (int i = 0; i < 256; ++i)
    {
        EXPECT_EQ(A.NextUInt32(), B.NextUInt32());
    }
}

TEST(MathTests, RandomStream_DifferentSequencesDiverge)
{
    Lumina::FRandomStream A(12345u, 1u);
    Lumina::FRandomStream B(12345u, 2u);

    int Differences = 0;
    for (int i = 0; i < 256; ++i)
    {
        Differences += (A.NextUInt32() != B.NextUInt32()) ? 1 : 0;
    }

    // Same seed, different stream selector: the two must not walk the same sequence.
    EXPECT_GT(Differences, 250);
}

TEST(MathTests, RandomStream_NextFloatIsUnitInterval)
{
    Lumina::FRandomStream Stream(9876u);
    for (int i = 0; i < 4096; ++i)
    {
        const float v = Stream.NextFloat();
        EXPECT_GE(v, 0.0f);
        EXPECT_LT(v, 1.0f);
    }
}

TEST(MathTests, RandomStream_SignedRangeSpanningZero)
{
    Lumina::FRandomStream Stream(555u);

    bool bSawNegative = false;
    bool bSawPositive = false;
    for (int i = 0; i < 2000; ++i)
    {
        const int32 v = Stream.RandRange<int32>(-10, 10);
        ASSERT_GE(v, -10);
        ASSERT_LE(v, 10);
        bSawNegative |= (v < 0);
        bSawPositive |= (v > 0);
    }

    // The unsigned span arithmetic has to survive a range straddling zero.
    EXPECT_TRUE(bSawNegative);
    EXPECT_TRUE(bSawPositive);
}

TEST(MathTests, RandomStream_FullWidthRangeDoesNotDegenerate)
{
    Lumina::FRandomStream Stream(31337u);

    // Span+1 overflows 32 bits here, which is the branch that has to fall back to the 64-bit path.
    bool bSawDistinct = false;
    const uint64 First = Stream.RandRange<uint64>(0u, UINT64_MAX);
    for (int i = 0; i < 64 && !bSawDistinct; ++i)
    {
        bSawDistinct = (Stream.RandRange<uint64>(0u, UINT64_MAX) != First);
    }

    EXPECT_TRUE(bSawDistinct);
}

TEST(MathTests, RandomStream_BoundedDrawCoversWholeRange)
{
    Lumina::FRandomStream Stream(4242u);

    // Every value in a small inclusive range must be reachable - an off-by-one in the Lemire bound
    // would silently drop the top or bottom value while every other assertion still passed.
    bool bSeen[5] = {};
    for (int i = 0; i < 4000; ++i)
    {
        const uint32 v = Stream.RandRange<uint32>(0u, 4u);
        ASSERT_LE(v, 4u);
        bSeen[v] = true;
    }

    for (const bool bValue : bSeen)
    {
        EXPECT_TRUE(bValue);
    }
}