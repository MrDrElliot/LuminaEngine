#include <gtest/gtest.h>

#include "Containers/Vector.h"
#include "Core/Math/SIMD/PixelOps.h"
#include "Platform/GenericPlatform.h"

using namespace Lumina;

namespace
{
    // Counts either side of both wide steps and their guards, so the tail runs at every offset.
    constexpr size_t kCounts[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 64, 1021 };

    // Padding keeps a guard bug reading allocated memory, so it shows as wrong output not a crash.
    constexpr size_t kSourcePadPixels = 8;
}

TEST(PixelOps, ExpandRGBToRGBA8MatchesScalar)
{
    for (size_t Count : kCounts)
    {
        TVector<uint8> Src(Lumina::IntCastChecked<Lumina::int32>((Count + kSourcePadPixels) * 3));
        for (int32 i = 0; i < Src.Num(); ++i)
        {
            Src[i] = (uint8)((i * 37 + 11) & 0xFF);
        }

        TVector<uint8> Want(Lumina::IntCastChecked<Lumina::int32>(Count * 4));
        for (int32 i = 0, Limit = Lumina::IntCastChecked<Lumina::int32>(Count); i < Limit; ++i)
        {
            Want[i * 4 + 0] = Src[i * 3 + 0];
            Want[i * 4 + 1] = Src[i * 3 + 1];
            Want[i * 4 + 2] = Src[i * 3 + 2];
            Want[i * 4 + 3] = 0xFFu;
        }

        TVector<uint8> Got(Lumina::IntCastChecked<Lumina::int32>(Count * 4), (uint8)0xAB);
        SIMD::ExpandRGBToRGBA8(Src.GetData(), Got.GetData(), Count);

        for (int32 i = 0, Limit = Lumina::IntCastChecked<Lumina::int32>(Count * 4); i < Limit; ++i)
        {
            ASSERT_EQ(Got[i], Want[i]) << "count " << Count << " byte " << i;
        }
    }
}

TEST(PixelOps, ExpandRGBToRGBA16MatchesScalar)
{
    for (size_t Count : kCounts)
    {
        TVector<uint16> Src(Lumina::IntCastChecked<Lumina::int32>((Count + kSourcePadPixels) * 3));
        for (int32 i = 0; i < Src.Num(); ++i)
        {
            Src[i] = (uint16)((i * 2477 + 13) & 0xFFFF);
        }

        TVector<uint16> Want(Lumina::IntCastChecked<Lumina::int32>(Count * 4));
        for (int32 i = 0, Limit = Lumina::IntCastChecked<Lumina::int32>(Count); i < Limit; ++i)
        {
            Want[i * 4 + 0] = Src[i * 3 + 0];
            Want[i * 4 + 1] = Src[i * 3 + 1];
            Want[i * 4 + 2] = Src[i * 3 + 2];
            Want[i * 4 + 3] = 0xFFFFu;
        }

        TVector<uint16> Got(Lumina::IntCastChecked<Lumina::int32>(Count * 4), (uint16)0xABCD);
        SIMD::ExpandRGBToRGBA16(Src.GetData(), Got.GetData(), Count);

        for (int32 i = 0, Limit = Lumina::IntCastChecked<Lumina::int32>(Count * 4); i < Limit; ++i)
        {
            ASSERT_EQ(Got[i], Want[i]) << "count " << Count << " element " << i;
        }
    }
}

// The wide step reads 16 bytes but consumes 12, so an over-read is invisible to an output check.
// Exact-sized buffers put it in reach of a sanitizer or page-heap build instead.
TEST(PixelOps, ExpandDoesNotReadPastAnExactlySizedSource)
{
    for (size_t Count : kCounts)
    {
        TVector<uint8> Src8(Lumina::IntCastChecked<Lumina::int32>(Count * 3), (uint8)0x5A);
        TVector<uint8> Dst8(Lumina::IntCastChecked<Lumina::int32>(Count * 4));
        SIMD::ExpandRGBToRGBA8(Src8.GetData(), Dst8.GetData(), Count);

        TVector<uint16> Src16(Lumina::IntCastChecked<Lumina::int32>(Count * 3), (uint16)0x5A5A);
        TVector<uint16> Dst16(Lumina::IntCastChecked<Lumina::int32>(Count * 4));
        SIMD::ExpandRGBToRGBA16(Src16.GetData(), Dst16.GetData(), Count);

        for (int32 i = 0, Limit = Lumina::IntCastChecked<Lumina::int32>(Count); i < Limit; ++i)
        {
            ASSERT_EQ(Dst8[i * 4 + 3], (uint8)0xFFu) << "count " << Count;
            ASSERT_EQ(Dst16[i * 4 + 3], (uint16)0xFFFFu) << "count " << Count;
        }
    }
}

TEST(PixelOps, ExpandWritesNothingPastTheLastPixel)
{
    constexpr size_t Count = 17;
    constexpr size_t Guard = 16;

    TVector<uint8> Src((Count + kSourcePadPixels) * 3, (uint8)0x5A);
    TVector<uint8> Dst(Count * 4 + Guard, (uint8)0xAB);

    SIMD::ExpandRGBToRGBA8(Src.GetData(), Dst.GetData(), Count);

    for (int32 i = Count * 4; i < Dst.Num(); ++i)
    {
        EXPECT_EQ(Dst[i], (uint8)0xAB) << "wrote past the last pixel at byte " << i;
    }
}
