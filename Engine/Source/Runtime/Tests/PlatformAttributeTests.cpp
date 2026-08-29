#include <gtest/gtest.h>

#include "Memory/Memory.h"
#include "Platform/Platform.h"

using namespace Lumina;

namespace
{
    // Compiles only while every attribute macro expands to something a declaration accepts, which is
    // what breaks first when one is added without a REFLECTION_PARSER mirror.
    LUMINA_FORCEINLINE_HINT int32 HintedAdd(int32 A, int32 B) { return A + B; }

    LUMINA_FORCEINLINE_DEBUGGABLE int32 DebuggableAdd(int32 A, int32 B) { return A + B; }

    FORCEINLINE int32 ForcedAdd(int32 A, int32 B) { return A + B; }

    LUMINA_HOT int32 HotAdd(int32 A, int32 B) { return A + B; }

    LUMINA_COLD int32 ColdAdd(int32 A, int32 B) { return A + B; }

    LUMINA_ALLOCATION LUMINA_RESTRICT_RETURN void* TestAllocate(size_t Size) LUMINA_ALLOC_SIZE(1);

    void* TestAllocate(size_t Size)
    {
        return Memory::Malloc(Size, 16);
    }

    struct FHolder
    {
        int32 Value = 7;

        NODISCARD const int32& Get() const LUMINA_LIFETIMEBOUND { return Value; }
    };
}

// The policy is the target's to choose, so this pins the contract rather than the choice.
TEST(PlatformAttributes, TheInlineHintPolicyIsWellFormed)
{
    constexpr int32 Policy = LUMINA_FORCEINLINE_HINTS_FORCED;
    EXPECT_TRUE(Policy == 0 || Policy == 1) << "a mistyped define would read as forced";
}

TEST(PlatformAttributes, AnnotatedFunctionsStillBehave)
{
    EXPECT_EQ(HintedAdd(2, 3), 5);
    EXPECT_EQ(DebuggableAdd(2, 3), 5);
    EXPECT_EQ(ForcedAdd(2, 3), 5);
    EXPECT_EQ(HotAdd(2, 3), 5);
    EXPECT_EQ(ColdAdd(2, 3), 5);

    const FHolder Holder;
    EXPECT_EQ(Holder.Get(), 7);
}

// The restrict and alloc_size attributes are a promise to the optimizer, so a wrong one shows up here.
TEST(PlatformAttributes, AnAnnotatedAllocationRoundTrips)
{
    void* Block = TestAllocate(256);
    ASSERT_NE(Block, nullptr);

    uint8* Bytes = static_cast<uint8*>(Block);
    for (int32 Index = 0; Index < 256; ++Index)
    {
        Bytes[Index] = static_cast<uint8>(Index);
    }
    for (int32 Index = 0; Index < 256; ++Index)
    {
        ASSERT_EQ(Bytes[Index], static_cast<uint8>(Index)) << "byte " << Index;
    }

    EXPECT_GE(Memory::GetAllocationSize(Block), size_t(256));
    Memory::Free(Block);
}

TEST(PlatformAttributes, EngineAllocationIsUsableThroughItsAnnotatedDeclaration)
{
    void* Block = Memory::Malloc(64, 32);
    ASSERT_NE(Block, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(Block) % 32u, 0u) << "the requested alignment still holds";

    void* Grown = Memory::Realloc(Block, 512, 32);
    ASSERT_NE(Grown, nullptr);
    Memory::Free(Grown);
}

// MSVC has no expression-level branch hint, so the statement form is the only one that reaches it.
TEST(PlatformAttributes, BranchHintsAreLiveOnThisCompiler)
{
#if defined(_MSC_VER) && !defined(__clang__)
    EXPECT_EQ(LUMINA_HAS_BRANCH_ATTRIBUTES, 1)
        << "without the C++20 attributes a Windows build gets no branch hint at all";
#endif

    int32 Taken = 0;

    LUMINA_LIKELY_IF (Taken == 0)
    {
        Taken = 1;
    }
    else
    {
        Taken = -1;
    }
    EXPECT_EQ(Taken, 1) << "the hint must not change which branch runs";

    LUMINA_UNLIKELY_IF (Taken == 99)
    {
        Taken = -1;
    }
    EXPECT_EQ(Taken, 1);

    // The expression form still has to evaluate its operand exactly once.
    int32 Calls = 0;
    auto Bump = [&Calls] { ++Calls; return true; };

    if (LIKELY(Bump()))
    {
        EXPECT_EQ(Calls, 1);
    }
    if (UNLIKELY(!Bump()))
    {
        FAIL() << "UNLIKELY inverted its condition";
    }
    EXPECT_EQ(Calls, 2);
}
