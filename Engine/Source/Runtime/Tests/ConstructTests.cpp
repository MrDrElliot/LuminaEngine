#include <gtest/gtest.h>

#include "Memory/Construct.h"

using namespace Lumina;

namespace
{
    struct FConstexprValue
    {
        constexpr explicit FConstexprValue(int32 InValue) : Value(InValue) {}
        constexpr ~FConstexprValue() {}

        int32 Value;
    };

    // Union storage rather than std::allocator, so the constexpr proof pulls in no standard library.
    union FConstexprSlot
    {
        char            Dummy;
        FConstexprValue Value;

        constexpr FConstexprSlot() : Dummy(0) {}
        constexpr ~FConstexprSlot() {}
    };

    constexpr int32 SumConstructedRange(int32 Count)
    {
        FConstexprSlot Storage[4];

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Memory::ConstructAt(&Storage[Index].Value, Index + 1);
        }

        int32 Sum = 0;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Sum += Storage[Index].Value.Value;
        }

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Memory::DestroyAt(&Storage[Index].Value);
        }
        return Sum;
    }

    static_assert(SumConstructedRange(4) == 10);

    constexpr int32 DestroyAtEndsLifetime()
    {
        FConstexprSlot Storage;

        Memory::ConstructAt(&Storage.Value, 7);
        const int32 First = Storage.Value.Value;
        Memory::DestroyAt(&Storage.Value);

        Memory::ConstructAt(&Storage.Value, 9);
        const int32 Second = Storage.Value.Value;
        Memory::DestroyAt(&Storage.Value);

        return First + Second;
    }

    static_assert(DestroyAtEndsLifetime() == 16);

    int32 GLiveCount = 0;

    struct FCounted
    {
        FCounted() : Tag(0) { ++GLiveCount; }
        explicit FCounted(int32 InTag) : Tag(InTag) { ++GLiveCount; }
        ~FCounted() { --GLiveCount; }

        int32 Tag;
    };

    struct FConstructFixture : public ::testing::Test
    {
        void SetUp() override { GLiveCount = 0; }
    };
}

TEST_F(FConstructFixture, ConstructAtForwardsArguments)
{
    alignas(FCounted) unsigned char Storage[sizeof(FCounted)];
    FCounted* Slot = reinterpret_cast<FCounted*>(Storage);

    FCounted* Constructed = Memory::ConstructAt(Slot, 42);
    EXPECT_EQ(Constructed, Slot);
    EXPECT_EQ(Slot->Tag, 42);
    EXPECT_EQ(GLiveCount, 1);

    Memory::DestroyAt(Slot);
    EXPECT_EQ(GLiveCount, 0);
}

TEST_F(FConstructFixture, ConstructAtDefaultConstructs)
{
    alignas(FCounted) unsigned char Storage[sizeof(FCounted)];
    FCounted* Slot = reinterpret_cast<FCounted*>(Storage);

    Memory::ConstructAt(Slot);
    EXPECT_EQ(Slot->Tag, 0);
    EXPECT_EQ(GLiveCount, 1);

    Memory::DestroyAt(Slot);
    EXPECT_EQ(GLiveCount, 0);
}

TEST_F(FConstructFixture, DestroyNRunsEveryDestructor)
{
    constexpr SIZE_T Count = 5;
    alignas(FCounted) unsigned char Storage[sizeof(FCounted) * Count];
    FCounted* First = reinterpret_cast<FCounted*>(Storage);

    for (SIZE_T Index = 0; Index < Count; ++Index)
    {
        Memory::ConstructAt(First + Index, static_cast<int32>(Index));
    }
    EXPECT_EQ(GLiveCount, static_cast<int32>(Count));

    Memory::DestroyN(First, Count);
    EXPECT_EQ(GLiveCount, 0);
}

TEST_F(FConstructFixture, DestroyRangeMatchesDestroyN)
{
    constexpr SIZE_T Count = 3;
    alignas(FCounted) unsigned char Storage[sizeof(FCounted) * Count];
    FCounted* First = reinterpret_cast<FCounted*>(Storage);

    for (SIZE_T Index = 0; Index < Count; ++Index)
    {
        Memory::ConstructAt(First + Index, static_cast<int32>(Index));
    }

    Memory::DestroyRange(First, First + Count);
    EXPECT_EQ(GLiveCount, 0);
}

TEST_F(FConstructFixture, DestroyNOnTrivialTypeIsANoOp)
{
    int32 Values[4] = { 1, 2, 3, 4 };

    Memory::DestroyN(Values, 4);
    EXPECT_EQ(Values[0], 1);
    EXPECT_EQ(Values[3], 4);
}
