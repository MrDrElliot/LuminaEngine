#include <gtest/gtest.h>

#include "Memory/DebugAllocator.h"

#if LUMINA_DEBUG_ALLOCATOR

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "Memory/Memory.h"

using namespace Lumina;

namespace DebugAllocatorTestsInternal
{
    using Memory::DebugAllocator::EError;
    using Memory::DebugAllocator::FError;

    struct FRecorded
    {
        EError          Kind;
        const void*     Pointer;
        int64           Offset;
        uint8           Expected;
        uint8           Found;
    };

    constexpr uint32 kMaxRecorded = 64;

    FRecorded GRecorded[kMaxRecorded];
    std::atomic<uint32> GRecordedCount{ 0 };

    void Record(const FError& Error)
    {
        const uint32 Index = GRecordedCount.fetch_add(1);
        if (Index < kMaxRecorded)
        {
            GRecorded[Index] = { Error.Kind, Error.Pointer, Error.Offset, Error.Expected, Error.Found };
        }
    }

    uint8* Allocate(size_t Size)
    {
        return static_cast<uint8*>(Memory::DebugAllocator::Allocate(Size, 16));
    }
}

class FDebugAllocatorTest : public ::testing::Test
{
protected:

    using EError = Memory::DebugAllocator::EError;

    void SetUp() override
    {
        Memory::DebugAllocator::FlushQuarantine();
        DebugAllocatorTestsInternal::GRecordedCount = 0;
        Previous = Memory::DebugAllocator::SetErrorHandler(&DebugAllocatorTestsInternal::Record);
    }

    void TearDown() override
    {
        Memory::DebugAllocator::SetErrorHandler(Previous);
    }

    static uint32 Count()
    {
        return DebugAllocatorTestsInternal::GRecordedCount.load();
    }

    static const DebugAllocatorTestsInternal::FRecorded& At(uint32 Index)
    {
        return DebugAllocatorTestsInternal::GRecorded[Index];
    }

    Memory::DebugAllocator::FErrorHandler Previous = nullptr;
};

TEST_F(FDebugAllocatorTest, CleanRoundTripReportsNothing)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(100);
    ASSERT_NE(Block, nullptr);
    for (int Index = 0; Index < 100; ++Index)
    {
        Block[Index] = static_cast<uint8>(Index);
    }
    Memory::DebugAllocator::Free(Block);
    Memory::DebugAllocator::FlushQuarantine();

    EXPECT_EQ(Count(), 0u);
}

TEST_F(FDebugAllocatorTest, MemoryWrappersRouteThroughIt)
{
    void* Block = Memory::Malloc(10);
    ASSERT_NE(Block, nullptr);
    EXPECT_EQ(Memory::GetAllocationSize(Block), 10u);
    EXPECT_FALSE(Memory::TryExpandInPlace(Block, 11));
    EXPECT_TRUE(Memory::TryExpandInPlace(Block, 10));

    void* Freed = Block;
    Memory::Free(Block);
    EXPECT_EQ(Block, nullptr);
    EXPECT_EQ(Count(), 0u);

    EXPECT_EQ(Memory::GetAllocationSize(Freed), 0u);
    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::FreedPointerUsed);
}

TEST_F(FDebugAllocatorTest, DoubleFreeIsReported)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(48);
    Memory::DebugAllocator::Free(Block);
    Memory::DebugAllocator::Free(Block);

    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::DoubleFree);
    EXPECT_EQ(At(0).Pointer, Block);
}

TEST_F(FDebugAllocatorTest, OverrunByOneByteIsReportedOnFree)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(24);
    Block[24] = 0;
    Memory::DebugAllocator::Free(Block);

    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::Overrun);
    EXPECT_EQ(At(0).Offset, 24);
    EXPECT_EQ(At(0).Expected, 0xFD);
    EXPECT_EQ(At(0).Found, 0);
}

TEST_F(FDebugAllocatorTest, UnderrunIsReportedOnFree)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(32);
    Block[-1] = 0x11;
    Memory::DebugAllocator::Free(Block);

    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::Underrun);
    EXPECT_EQ(At(0).Offset, -1);
    EXPECT_EQ(At(0).Found, 0x11);
}

TEST_F(FDebugAllocatorTest, WriteAfterFreeIsReportedWhenTheQuarantineReleasesIt)
{
    if (Memory::DebugAllocator::GetStats().bPageGuard)
    {
        GTEST_SKIP() << "a page-guarded heap faults on the write itself";
    }

    uint8* Block = DebugAllocatorTestsInternal::Allocate(64);
    Memory::DebugAllocator::Free(Block);
    Block[7] = 0x42;
    Memory::DebugAllocator::FlushQuarantine();

    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::UseAfterFree);
    EXPECT_EQ(At(0).Offset, 7);
    EXPECT_EQ(At(0).Found, 0x42);
}

TEST_F(FDebugAllocatorTest, FreeingAnInteriorOrForeignPointerIsReported)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(64);
    Memory::DebugAllocator::Free(Block + 16);

    int OnTheStack = 0;
    Memory::DebugAllocator::Free(&OnTheStack);

    ASSERT_EQ(Count(), 2u);
    EXPECT_EQ(At(0).Kind, EError::InvalidPointer);
    EXPECT_EQ(At(0).Pointer, Block + 16);
    EXPECT_EQ(At(1).Kind, EError::InvalidPointer);

    Memory::DebugAllocator::Free(Block);
    EXPECT_EQ(Count(), 2u);
}

TEST_F(FDebugAllocatorTest, HeaderDamageIsReportedAndTheBlockIsNotReleased)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(16);
    Block[-40] ^= 0xFF;
    Memory::DebugAllocator::Free(Block);

    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::HeaderCorrupted);

    Memory::DebugAllocator::Free(Block);
    ASSERT_EQ(Count(), 2u);
    EXPECT_EQ(At(1).Kind, EError::InvalidPointer);
}

TEST_F(FDebugAllocatorTest, ReallocateAlwaysMovesAndKeepsTheContents)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(32);
    for (int Index = 0; Index < 32; ++Index)
    {
        Block[Index] = static_cast<uint8>(Index * 3);
    }

    uint8* Grown = static_cast<uint8*>(Memory::DebugAllocator::Reallocate(Block, 64, 16));
    ASSERT_NE(Grown, nullptr);
    EXPECT_NE(Grown, Block);
    for (int Index = 0; Index < 32; ++Index)
    {
        EXPECT_EQ(Grown[Index], static_cast<uint8>(Index * 3));
    }

    uint8* Shrunk = static_cast<uint8*>(Memory::DebugAllocator::Reallocate(Grown, 8, 16));
    ASSERT_NE(Shrunk, nullptr);
    EXPECT_NE(Shrunk, Grown);
    EXPECT_EQ(Memory::DebugAllocator::GetAllocationSize(Shrunk), 8u);
    EXPECT_EQ(Shrunk[5], 15);
    EXPECT_EQ(Count(), 0u);

    EXPECT_EQ(Memory::DebugAllocator::GetAllocationSize(Block), 0u);
    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::FreedPointerUsed);

    Memory::DebugAllocator::Free(Shrunk);
}

TEST_F(FDebugAllocatorTest, ReallocateToZeroKeepsALiveBlock)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(32);
    void* Empty = Memory::DebugAllocator::Reallocate(Block, 0, 16);
    ASSERT_NE(Empty, nullptr);
    EXPECT_EQ(Memory::DebugAllocator::GetAllocationSize(Empty), 0u);
    Memory::DebugAllocator::Free(Empty);
    EXPECT_EQ(Count(), 0u);
}

TEST_F(FDebugAllocatorTest, GarbageSizeIsRefusedRatherThanAllocated)
{
    EXPECT_EQ(Memory::DebugAllocator::Allocate(3948752093457239045ull, 16), nullptr);
    EXPECT_EQ(Memory::DebugAllocator::Allocate(64, 24), nullptr);

    ASSERT_EQ(Count(), 2u);
    EXPECT_EQ(At(0).Kind, EError::InvalidArgument);
    EXPECT_EQ(At(1).Kind, EError::InvalidArgument);
}

TEST_F(FDebugAllocatorTest, AlignmentIsHonouredWithTheGuardsIntact)
{
    for (size_t Alignment : { size_t(16), size_t(32), size_t(64), size_t(256), size_t(4096) })
    {
        uint8* Block = static_cast<uint8*>(Memory::DebugAllocator::Allocate(40, Alignment));
        ASSERT_NE(Block, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(Block) % Alignment, 0u) << Alignment;
        std::memset(Block, 0x5A, 40);
        Memory::DebugAllocator::Free(Block);
    }
    Memory::DebugAllocator::FlushQuarantine();
    EXPECT_EQ(Count(), 0u);
}

TEST_F(FDebugAllocatorTest, ValidateHeapFindsLiveDamageOnceAndRepairsIt)
{
    uint8* Block = DebugAllocatorTestsInternal::Allocate(24);
    Block[27] = 0;

    EXPECT_EQ(Memory::DebugAllocator::ValidateHeap(), 1u);
    ASSERT_EQ(Count(), 1u);
    EXPECT_EQ(At(0).Kind, EError::Overrun);
    EXPECT_EQ(At(0).Offset, 27);

    EXPECT_EQ(Memory::DebugAllocator::ValidateHeap(), 0u);
    Memory::DebugAllocator::Free(Block);
    EXPECT_EQ(Count(), 1u);
}

TEST_F(FDebugAllocatorTest, ConcurrentChurnStaysConsistent)
{
    constexpr int kThreads = 8;
    constexpr int kRounds = 4000;

    std::vector<std::thread> Threads;
    for (int Worker = 0; Worker < kThreads; ++Worker)
    {
        Threads.emplace_back([Worker]
        {
            uint8* Held[16] = {};
            for (int Round = 0; Round < kRounds; ++Round)
            {
                const int Slot = (Round * 7 + Worker) % 16;
                if (Held[Slot] != nullptr)
                {
                    Memory::DebugAllocator::Free(Held[Slot]);
                }
                const size_t Size = static_cast<size_t>((Round * 31 + Worker * 13) % 700);
                Held[Slot] = DebugAllocatorTestsInternal::Allocate(Size);
                std::memset(Held[Slot], Worker, Size);
                if (Round % 3 == 0)
                {
                    Held[Slot] = static_cast<uint8*>(Memory::DebugAllocator::Reallocate(Held[Slot], Size + 40, 16));
                }
            }
            for (uint8* Block : Held)
            {
                Memory::DebugAllocator::Free(Block);
            }
        });
    }
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }

    EXPECT_EQ(Memory::DebugAllocator::ValidateHeap(), 0u);
    Memory::DebugAllocator::FlushQuarantine();
    EXPECT_EQ(Count(), 0u);
}

#endif
