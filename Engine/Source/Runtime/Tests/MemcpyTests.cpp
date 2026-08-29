#include <gtest/gtest.h>
#include <vector>
#include "Memory/Memcpy.h"
#include "Memory/Memory.h"

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

namespace
{
    class FSetHarness
    {
    public:

        explicit FSetHarness(size_t Capacity)
            : Arena(Capacity + 2 * kGuard + 64)
        {
        }

        testing::AssertionResult Run(size_t Size, size_t DestOffset, uint8 Value)
        {
            std::fill(Arena.begin(), Arena.end(), static_cast<uint8>(0xAB));

            uint8* Dest = Arena.data() + kGuard + DestOffset;
            Memory::Memset(Dest, Value, Size);

            for (size_t Index = 0; Index < Size; ++Index)
            {
                if (Dest[Index] != Value)
                {
                    return testing::AssertionFailure()
                        << "wrong byte at " << Index << " Size=" << Size << " Offset=" << DestOffset;
                }
            }

            for (size_t Index = 0; Index < kGuard + DestOffset; ++Index)
            {
                if (Arena[Index] != 0xAB)
                {
                    return testing::AssertionFailure() << "underrun Size=" << Size << " Offset=" << DestOffset;
                }
            }

            for (size_t Index = kGuard + DestOffset + Size; Index < Arena.size(); ++Index)
            {
                if (Arena[Index] != 0xAB)
                {
                    return testing::AssertionFailure() << "overrun Size=" << Size << " Offset=" << DestOffset;
                }
            }

            return testing::AssertionSuccess();
        }

    private:

        std::vector<uint8> Arena;
    };

    int32 Sign(int32 Value)
    {
        return Value < 0 ? -1 : (Value > 0 ? 1 : 0);
    }
}

TEST(MemsetTest, EverySizeAndAlignmentMatchesTheValue)
{
    FSetHarness Harness(2048);

    for (size_t Size = 0; Size <= 2048; ++Size)
    {
        for (size_t Offset = 0; Offset < 40; Offset += 9)
        {
            ASSERT_TRUE(Harness.Run(Size, Offset, 0x5A));
        }
    }
}

TEST(MemsetTest, BoundarySizesAtEveryAlignmentAndValue)
{
    FSetHarness Harness(8192);

    const size_t Sizes[] = { 0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
                             255, 256, 511, 512, 1023, 1024, 4095, 4096, 4097, 8192 };

    for (size_t Size : Sizes)
    {
        for (size_t Offset = 0; Offset < 32; ++Offset)
        {
            for (uint8 Value : { uint8(0), uint8(0xFF), uint8(0x7F) })
            {
                ASSERT_TRUE(Harness.Run(Size, Offset, Value));
            }
        }
    }
}

TEST(MemcmpTest, MatchesTheCrtSignForEveryDifferencePosition)
{
    for (size_t Size = 1; Size <= 600; ++Size)
    {
        std::vector<uint8> A(Size + 64), B(Size + 64);
        for (size_t Index = 0; Index < A.size(); ++Index)
        {
            A[Index] = PatternByte(Index);
            B[Index] = PatternByte(Index);
        }

        for (size_t Offset : { size_t(0), size_t(1), size_t(7), size_t(16), size_t(17) })
        {
            ASSERT_EQ(Sign(Memory::Memcmp(A.data() + Offset, B.data() + Offset, Size)), 0)
                << "equal buffers differed, Size=" << Size;

            for (size_t Position = 0; Position < Size; Position += (Size > 64 ? 7 : 1))
            {
                const uint8 Saved = B[Offset + Position];

                B[Offset + Position] = static_cast<uint8>(Saved + 1);
                ASSERT_EQ(Sign(Memory::Memcmp(A.data() + Offset, B.data() + Offset, Size)),
                          Sign(std::memcmp(A.data() + Offset, B.data() + Offset, Size)))
                    << "Size=" << Size << " Offset=" << Offset << " Position=" << Position;

                B[Offset + Position] = static_cast<uint8>(Saved - 1);
                ASSERT_EQ(Sign(Memory::Memcmp(A.data() + Offset, B.data() + Offset, Size)),
                          Sign(std::memcmp(A.data() + Offset, B.data() + Offset, Size)))
                    << "Size=" << Size << " Offset=" << Offset << " Position=" << Position;

                B[Offset + Position] = Saved;
            }
        }
    }
}

TEST(MemcmpTest, UsesUnsignedByteOrderingLikeTheCrt)
{
    const uint8 A[] = { 0x00, 0x80 };
    const uint8 B[] = { 0x00, 0x7F };

    EXPECT_EQ(Sign(Memory::Memcmp(A, B, 2)), Sign(std::memcmp(A, B, 2)));
    EXPECT_GT(Memory::Memcmp(A, B, 2), 0);
}

TEST(MemcmpTest, LargeBuffersPastTheVectorLimit)
{
    const size_t Size = 64 * 1024;
    std::vector<uint8> A(Size), B(Size);
    for (size_t Index = 0; Index < Size; ++Index)
    {
        A[Index] = PatternByte(Index);
        B[Index] = PatternByte(Index);
    }

    EXPECT_EQ(Memory::Memcmp(A.data(), B.data(), Size), 0);

    B[Size - 1] ^= 0xFF;
    EXPECT_EQ(Sign(Memory::Memcmp(A.data(), B.data(), Size)),
              Sign(std::memcmp(A.data(), B.data(), Size)));
}

TEST(MemcmpTest, ZeroSizeIsAlwaysEqual)
{
    const uint8 A[] = { 1 };
    const uint8 B[] = { 2 };
    EXPECT_EQ(Memory::Memcmp(A, B, 0), 0);
}
