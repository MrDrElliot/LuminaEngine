#include <gtest/gtest.h>

#include "Types/BitFlags.h"

using namespace Lumina;

namespace
{
    enum class ETestFlag : uint8
    {
        First = 0,
        Second = 1,
    };
}

TEST(BitFlags, OrAndAndDoNotMutateLeftOperand)
{
    FBitFlags A(0b01);

    FBitFlags Combined = A | uint8(1);
    EXPECT_EQ(A.Get(), 0b01u);
    EXPECT_EQ(Combined.Get(), 0b11u);

    FBitFlags Masked = Combined & uint8(1);
    EXPECT_EQ(Combined.Get(), 0b11u);
    EXPECT_EQ(Masked.Get(), 0b10u);
}

TEST(TBitFlags, EnumConstructorSetsThatBit)
{
    TBitFlags<ETestFlag> First(ETestFlag::First);
    EXPECT_EQ(First.Get(), 0b01u);
    EXPECT_TRUE(First.IsFlagSet(ETestFlag::First));

    TBitFlags<ETestFlag> Second(ETestFlag::Second);
    EXPECT_EQ(Second.Get(), 0b10u);
    EXPECT_FALSE(Second.IsFlagSet(ETestFlag::First));
}

TEST(TBitFlags, OrAndAndDoNotMutateLeftOperand)
{
    TBitFlags<ETestFlag> A;
    A.SetFlag(ETestFlag::First);

    TBitFlags<ETestFlag> Combined = A | ETestFlag::Second;
    EXPECT_TRUE(A.IsFlagSet(ETestFlag::First));
    EXPECT_FALSE(A.IsFlagSet(ETestFlag::Second));
    EXPECT_TRUE(Combined.IsFlagSet(ETestFlag::First));
    EXPECT_TRUE(Combined.IsFlagSet(ETestFlag::Second));

    TBitFlags<ETestFlag> Masked = Combined & ETestFlag::Second;
    EXPECT_TRUE(Combined.IsFlagSet(ETestFlag::First));
    EXPECT_TRUE(Combined.IsFlagSet(ETestFlag::Second));
    EXPECT_FALSE(Masked.IsFlagSet(ETestFlag::First));
    EXPECT_TRUE(Masked.IsFlagSet(ETestFlag::Second));
}
