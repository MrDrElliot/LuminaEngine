#include <gtest/gtest.h>

#include <memory>

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

    // The whole point of routing through std::construct_at, so it is asserted at compile time.
    constexpr int32 SumConstructedRange(int32 Count)
    {
        std::allocator<FConstexprValue> Allocator;
        FConstexprValue* Buffer = Allocator.allocate(static_cast<size_t>(Count));

        for (int32 Index = 0; Index < Count; ++Index)
        {
            Memory::ConstructAt(Buffer + Index, Index + 1);
        }

        int32 Sum = 0;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Sum += Buffer[Index].Value;
        }

        Memory::DestroyN(Buffer, static_cast<size_t>(Count));
        Allocator.deallocate(Buffer, static_cast<size_t>(Count));
        return Sum;
    }

    static_assert(SumConstructedRange(4) == 10);

    constexpr int32 DestroyAtEndsLifetime()
    {
        std::allocator<FConstexprValue> Allocator;
        FConstexprValue* Slot = Allocator.allocate(1);

        Memory::ConstructAt(Slot, 7);
        const int32 First = Slot->Value;
        Memory::DestroyAt(Slot);

        Memory::ConstructAt(Slot, 9);
        const int32 Second = Slot->Value;
        Memory::DestroyAt(Slot);

        Allocator.deallocate(Slot, 1);
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
    constexpr size_t Count = 5;
    alignas(FCounted) unsigned char Storage[sizeof(FCounted) * Count];
    FCounted* First = reinterpret_cast<FCounted*>(Storage);

    for (size_t Index = 0; Index < Count; ++Index)
    {
        Memory::ConstructAt(First + Index, static_cast<int32>(Index));
    }
    EXPECT_EQ(GLiveCount, static_cast<int32>(Count));

    Memory::DestroyN(First, Count);
    EXPECT_EQ(GLiveCount, 0);
}

TEST_F(FConstructFixture, DestroyRangeMatchesDestroyN)
{
    constexpr size_t Count = 3;
    alignas(FCounted) unsigned char Storage[sizeof(FCounted) * Count];
    FCounted* First = reinterpret_cast<FCounted*>(Storage);

    for (size_t Index = 0; Index < Count; ++Index)
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
