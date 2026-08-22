#include <gtest/gtest.h>
#include "Platform/Time/PlatformTime.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Memory/Allocators/ScratchArray.h"
#include "TaskSystem/TaskSystem.h"
#include <atomic>
#include <cstdio>
#include <vector>

using namespace Lumina;

namespace
{
    struct FTrivial       { float X; int32 Y; };
    struct FUserCtor      { int32 X; FUserCtor(int32 In) : X(In) {} };
    struct FNonTrivialDtor{ int32 X; ~FNonTrivialDtor() {} };
    struct FVirtual       { virtual ~FVirtual() = default; };
}

static_assert(ScratchElement<float>);
static_assert(ScratchElement<int32>);
static_assert(ScratchElement<FTrivial>);
static_assert(ScratchElement<FVector3>);
static_assert(ScratchElement<float[4]>);

static_assert(!ScratchElement<void>);
static_assert(!ScratchElement<float&>);
static_assert(!ScratchElement<const float>);
static_assert(!ScratchElement<volatile float>);
static_assert(!ScratchElement<float[]>);
static_assert(!ScratchElement<FUserCtor>);
static_assert(!ScratchElement<FNonTrivialDtor>);
static_assert(!ScratchElement<FVirtual>);
static_assert(!ScratchElement<TVector<float>>);
static_assert(!ScratchElement<FString>);

TEST(ScratchArray, DefaultIsEmptyAndHoldsNothing)
{
    TScratchArray<float> Array;
    EXPECT_EQ(Array.Num(), 0u);
    EXPECT_TRUE(Array.IsEmpty());
    EXPECT_EQ(Array.GetData(), nullptr);
}

TEST(ScratchArray, SizesAndIsWritableEndToEnd)
{
    constexpr SIZE_T Count = 262144;

    TScratchArray<float> Array(Count);
    ASSERT_EQ(Array.Num(), Count);
    ASSERT_NE(Array.GetData(), nullptr);

    for (SIZE_T i = 0; i < Count; ++i)
    {
        Array[i] = (float)i;
    }

    EXPECT_FLOAT_EQ(Array[0], 0.0f);
    EXPECT_FLOAT_EQ(Array[Count - 1], (float)(Count - 1));
}

TEST(ScratchArray, BlocksAreSixtyFourByteAligned)
{
    for (SIZE_T Count : {1u, 1024u, 262144u})
    {
        TScratchArray<float> Array(Count);
        EXPECT_EQ((uintptr_t)Array.GetData() % 64u, 0u) << "count " << Count;
    }
}

TEST(ScratchArray, FillWritesEveryElement)
{
    TScratchArray<int32> Array(1000);
    Array.Fill(-1);

    for (int32 Value : Array)
    {
        ASSERT_EQ(Value, -1);
    }
}

// The point of the pool is that a same-sized reacquire reuses a retired block.
TEST(ScratchArray, ReleasedBlockIsReusedBySameSizeRequest)
{
    ScratchPool::Trim();

    const void* First = nullptr;
    {
        TScratchArray<float> Array(262144);
        First = Array.GetData();
    }

    EXPECT_EQ(ScratchPool::GetStats().RetainedBlocks, 1u);

    TScratchArray<float> Second(262144);
    EXPECT_EQ((const void*)Second.GetData(), First);
    EXPECT_EQ(ScratchPool::GetStats().RetainedBlocks, 0u);
}

TEST(ScratchArray, ResetKeepsABlockThatAlreadyFits)
{
    TScratchArray<float> Array(262144);
    const void* Original = Array.GetData();

    Array.Reset(1024);
    EXPECT_EQ(Array.Num(), 1024u);
    EXPECT_EQ((const void*)Array.GetData(), Original);

    Array.Reset(262144);
    EXPECT_EQ((const void*)Array.GetData(), Original);
}

TEST(ScratchArray, ResetToZeroKeepsTheBlockButReportsEmpty)
{
    TScratchArray<float> Array(4096);
    const void* Original = Array.GetData();

    Array.Reset(0);
    EXPECT_TRUE(Array.IsEmpty());
    EXPECT_EQ((const void*)Array.GetData(), Original);
}

TEST(ScratchArray, MoveTransfersOwnershipExactlyOnce)
{
    ScratchPool::Trim();

    {
        TScratchArray<float> Source(4096);
        const void* Data = Source.GetData();

        TScratchArray<float> Moved = Move(Source);
        EXPECT_EQ((const void*)Moved.GetData(), Data);
        EXPECT_EQ(Moved.Num(), 4096u);
        EXPECT_TRUE(Source.IsEmpty());
        EXPECT_EQ(Source.GetData(), nullptr);
    }

    EXPECT_EQ(ScratchPool::GetStats().LiveBlocks, 0u);
    EXPECT_EQ(ScratchPool::GetStats().RetainedBlocks, 1u);
}

TEST(ScratchArray, MoveAssignReleasesTheOverwrittenBlock)
{
    ScratchPool::Trim();

    {
        TScratchArray<float> A(4096);
        TScratchArray<float> B(4096);

        A = Move(B);
        EXPECT_EQ(A.Num(), 4096u);
    }

    EXPECT_EQ(ScratchPool::GetStats().LiveBlocks, 0u);
}

TEST(ScratchArray, TrimFreesRetainedBlocks)
{
    {
        TScratchArray<float> Array(262144);
        (void)Array.GetData();
    }

    ScratchPool::Trim();

    const ScratchPool::FStats Stats = ScratchPool::GetStats();
    EXPECT_EQ(Stats.RetainedBlocks, 0u);
    EXPECT_EQ(Stats.RetainedBytes, 0u);
}

// A size past the largest class must still work; it goes direct to the allocator and is never retained.
TEST(ScratchArray, OversizedRequestBypassesThePool)
{
    ScratchPool::Trim();

    constexpr SIZE_T HugeFloats = ((SIZE_T)4 * 1024 * 1024 * 1024) / sizeof(float);
    {
        TScratchArray<float> Array(HugeFloats / 2048);
        ASSERT_NE(Array.GetData(), nullptr);
        Array[0] = 1.0f;
        EXPECT_FLOAT_EQ(Array[0], 1.0f);
    }

    EXPECT_EQ(ScratchPool::GetStats().LiveBlocks, 0u);
}

// Blocks are owned by the object, not the thread, so acquiring and releasing across workers must balance.
TEST(ScratchArray, ConcurrentAcquireReleaseBalances)
{
    ScratchPool::Trim();

    constexpr uint32 Iterations = 512;
    std::atomic<uint64> Checksum{0};

    Task::ParallelFor(Iterations, [&](uint32 Index)
    {
        const SIZE_T Count = 1024u + (Index % 16u) * 4096u;

        TScratchArray<uint32> Array(Count);
        Array.Fill(Index);

        uint64 Local = 0;
        for (uint32 Value : Array)
        {
            Local += Value;
        }

        Checksum.fetch_add(Local, std::memory_order_relaxed);
    }, 1);

    uint64 Expected = 0;
    for (uint32 Index = 0; Index < Iterations; ++Index)
    {
        Expected += (uint64)Index * (uint64)(1024u + (Index % 16u) * 4096u);
    }

    EXPECT_EQ(Checksum.load(), Expected);
    EXPECT_EQ(ScratchPool::GetStats().LiveBlocks, 0u);

    ScratchPool::Trim();
}

// A block handed back on a different thread than it came from is the case a thread-scoped arena cannot do.
TEST(ScratchArray, BlockCrossesThreadsBetweenAcquireAndRelease)
{
    ScratchPool::Trim();

    std::vector<TScratchArray<float>> Arrays;
    Arrays.resize(32);

    Task::ParallelFor(32u, [&](uint32 Index)
    {
        Arrays[Index] = TScratchArray<float>(65536);
        Arrays[Index].Fill((float)Index);
    }, 1);

    EXPECT_EQ(ScratchPool::GetStats().LiveBlocks, 32u);

    for (uint32 Index = 0; Index < 32u; ++Index)
    {
        ASSERT_EQ(Arrays[Index].Num(), 65536u);
        EXPECT_FLOAT_EQ(Arrays[Index][0], (float)Index);
    }

    Arrays.clear();
    EXPECT_EQ(ScratchPool::GetStats().LiveBlocks, 0u);

    ScratchPool::Trim();
}

// Run with --gtest_also_run_disabled_tests. Sized to one 64^3 terrain chunk's three noise buffers.
TEST(ScratchArray, DISABLED_BenchAgainstVector)
{
    constexpr SIZE_T Count = 64 * 64 * 64;
    constexpr int32  Runs  = 200;

    const auto Now = [] { return Lumina::PlatformTime::Cycles(); };
    const auto Ms  = [](auto Start, auto End)
    {
        return Lumina::PlatformTime::ToMilliseconds(End - Start);
    };

    double Sink = 0.0;

    const auto VectorStart = Now();
    for (int32 Run = 0; Run < Runs; ++Run)
    {
        TVector<float> A(Count, 0.0f);
        TVector<float> B(Count, 0.0f);
        TVector<float> C(Count, 0.0f);
        A[Run] = 1.0f; B[Run] = 2.0f; C[Run] = 3.0f;
        Sink += A[Run] + B[Run] + C[Run];
    }
    const double VectorMs = Ms(VectorStart, Now());

    ScratchPool::Trim();

    const auto ScratchStart = Now();
    for (int32 Run = 0; Run < Runs; ++Run)
    {
        TScratchArray<float> A(Count);
        TScratchArray<float> B(Count);
        TScratchArray<float> C(Count);
        A[Run] = 1.0f; B[Run] = 2.0f; C[Run] = 3.0f;
        Sink += A[Run] + B[Run] + C[Run];
    }
    const double ScratchMs = Ms(ScratchStart, Now());

    std::printf("\n  3 x %llu floats, %d runs\n", (unsigned long long)Count, Runs);
    std::printf("  TVector      %8.3f ms  (%6.4f ms/chunk)\n", VectorMs, VectorMs / Runs);
    std::printf("  TScratchArray%8.3f ms  (%6.4f ms/chunk)  %.1fx\n",
                ScratchMs, ScratchMs / Runs, VectorMs / ScratchMs);
    std::printf("  retained after: %llu KiB\n\n",
                (unsigned long long)(ScratchPool::GetStats().RetainedBytes / 1024));

    EXPECT_GT(Sink, 0.0);
}
