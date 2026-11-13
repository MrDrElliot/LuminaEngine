#include <gtest/gtest.h>

#include "Cooker/CookDDC.h"

using namespace Lumina;

TEST(CookDDC, KeyChangesWithEngineVersion)
{
    constexpr uint64 SourceHash = 0x1234567890abcdefull;

    const FCookInputHash KeyA = FCookDDC::ComputeKey(SourceHash, 100);
    const FCookInputHash KeyB = FCookDDC::ComputeKey(SourceHash, 101);

    EXPECT_TRUE(KeyA.IsValid());
    EXPECT_TRUE(KeyB.IsValid());
    EXPECT_NE(KeyA, KeyB);
}

TEST(CookDDC, KeyIsStableForSameInputs)
{
    constexpr uint64 SourceHash = 0xdeadbeefcafef00dull;

    const FCookInputHash KeyA = FCookDDC::ComputeKey(SourceHash, 176);
    const FCookInputHash KeyB = FCookDDC::ComputeKey(SourceHash, 176);

    EXPECT_EQ(KeyA, KeyB);
}

TEST(CookDDC, ZeroSourceHashStaysInvalid)
{
    EXPECT_FALSE(FCookDDC::ComputeKey(0, 176).IsValid());
}
