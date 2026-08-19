#include <gtest/gtest.h>
#include <vector>
#include "Memory/Memcpy.h"

using namespace Lumina;

namespace
{
    constexpr size_t kGuard = 128;

    uint8 PatternByte(size_t Index)
    {
        return static_cast<uint8>((Index * 131u + 17u) & 0xFF);
    }

    // Destination sits inside a guarded arena so an over- or under-run is caught, not just a wrong copy.
    class FCopyHarness
    {
    public:

        explicit FCopyHarness(size_t Capacity)
            : Arena(Capacity + 2 * kGuard + 64)
            , Source(Capacity + 64)
        {
            for (size_t Index = 0; Index < Source.size(); ++Index)
            {
                Source[Index] = PatternByte(Index);
            }
        }

        testing::AssertionResult Run(size_t Size, size_t DestOffset, size_t SrcOffset)
        {
            std::fill(Arena.begin(), Arena.end(), static_cast<uint8>(0xAB));

            uint8* Dest = Arena.data() + kGuard + DestOffset;
            const uint8* Src = Source.data() + SrcOffset;

            Memory::Memcpy(Dest, Src, Size);

            for (size_t Index = 0; Index < Size; ++Index)
            {
                if (Dest[Index] != Src[Index])
                {
                    return testing::AssertionFailure()
                        << "wrong byte at " << Index << " for Size=" << Size
                        << " DestOffset=" << DestOffset << " SrcOffset=" << SrcOffset;
                }
            }

            for (size_t Index = 0; Index < kGuard + DestOffset; ++Index)
            {
                if (Arena[Index] != 0xAB)
                {
                    return testing::AssertionFailure()
                        << "underrun " << (kGuard + DestOffset - Index) << " bytes before dest, Size=" << Size
                        << " DestOffset=" << DestOffset << " SrcOffset=" << SrcOffset;
                }
            }

            const size_t TailStart = kGuard + DestOffset + Size;
            for (size_t Index = TailStart; Index < Arena.size(); ++Index)
            {
                if (Arena[Index] != 0xAB)
                {
                    return testing::AssertionFailure()
                        << "overrun " << (Index - TailStart) << " bytes past dest, Size=" << Size
                        << " DestOffset=" << DestOffset << " SrcOffset=" << SrcOffset;
                }
            }

            return testing::AssertionSuccess();
        }

    private:

        std::vector<uint8> Arena;
        std::vector<uint8> Source;
    };
}

TEST(MemcpyTest, EverySizeUpTo1024AtEveryAlignmentCombination)
{
    FCopyHarness Harness(1024);

    for (size_t Size = 0; Size <= 1024; ++Size)
    {
        for (size_t DestOffset = 0; DestOffset < 64; DestOffset += 7)
        {
            for (size_t SrcOffset = 0; SrcOffset < 64; SrcOffset += 5)
            {
                ASSERT_TRUE(Harness.Run(Size, DestOffset, SrcOffset));
            }
        }
    }
}

TEST(MemcpyTest, BoundarySizesAtEveryDestinationAlignment)
{
    FCopyHarness Harness(4096);

    const size_t Sizes[] =
    {
        0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 95, 96, 127, 128, 129,
        191, 192, 255, 256, 257, 319, 320, 383, 384, 511, 512, 1023, 1024,
        1025, 2047, 2048, 4095, 4096,
    };

    for (size_t Size : Sizes)
    {
        for (size_t DestOffset = 0; DestOffset < 64; ++DestOffset)
        {
            for (size_t SrcOffset : { size_t(0), size_t(1), size_t(15), size_t(16), size_t(31), size_t(32), size_t(33) })
            {
                ASSERT_TRUE(Harness.Run(Size, DestOffset, SrcOffset));
            }
        }
    }
}

TEST(MemcpyTest, BothPointersVectorAlignedTakesTheAlignedPath)
{
    FCopyHarness Harness(8192);

    for (size_t Size = 65; Size <= 8192; Size += 31)
    {
        ASSERT_TRUE(Harness.Run(Size, 0, 0));
        ASSERT_TRUE(Harness.Run(Size, 32, 32));
    }
}

TEST(MemcpyTest, LargeCopiesPastTheVectorLimit)
{
    const size_t Sizes[] = { 256 * 1024, 256 * 1024 + 1, 1024 * 1024, 3 * 1024 * 1024 + 777 };

    for (size_t Size : Sizes)
    {
        FCopyHarness Harness(Size);
        ASSERT_TRUE(Harness.Run(Size, 0, 0));
        ASSERT_TRUE(Harness.Run(Size, 5, 3));
    }
}

TEST(MemcpyTest, ZeroSizeTouchesNothing)
{
    FCopyHarness Harness(64);
    EXPECT_TRUE(Harness.Run(0, 0, 0));
    EXPECT_TRUE(Harness.Run(0, 13, 7));
}
