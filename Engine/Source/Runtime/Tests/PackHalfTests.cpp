#include <gtest/gtest.h>
#include "Core/Math/Math.h"
#include "Core/Math/Packing.h"
#include "Core/Math/SIMD/PackHalf.h"
#include <vector>

using namespace Lumina;

namespace
{
    std::vector<float> MakePairs()
    {
        std::vector<float> Pairs;
        const float Interesting[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 0.25f, 1.0f / 3.0f, 65504.0f,
                                      -65504.0f, 6.1e-5f, 1e-8f, 123.456f, -0.001953125f, 2048.0f };

        for (float A : Interesting)
        {
            for (float B : Interesting)
            {
                Pairs.push_back(A);
                Pairs.push_back(B);
            }
        }

        // An odd count so the vector tail is exercised.
        Pairs.push_back(7.5f);
        Pairs.push_back(-3.25f);
        return Pairs;
    }

    int32 HalfDistance(uint16 A, uint16 B)
    {
        return Math::Abs((int32)A - (int32)B);
    }
}

// The array path may use F16C, which rounds half to even where the scalar path rounds half up.
TEST(PackHalf, ArrayMatchesScalarWithinOneUlp)
{
    const std::vector<float> Pairs = MakePairs();
    const uint32 Count = (uint32)(Pairs.size() / 2);

    std::vector<uint32> Packed(Count, 0u);
    SIMD::PackHalf2x16Array(Pairs.data(), Packed.data(), Count);

    for (uint32 i = 0; i < Count; ++i)
    {
        const uint32 Expected = Math::PackHalf2x16(TVec<float, 2>(Pairs[i * 2], Pairs[i * 2 + 1]));

        EXPECT_LE(HalfDistance((uint16)(Packed[i] & 0xFFFFu), (uint16)(Expected & 0xFFFFu)), 1)
            << "x lane " << i << " (" << Pairs[i * 2] << ")";
        EXPECT_LE(HalfDistance((uint16)(Packed[i] >> 16), (uint16)(Expected >> 16)), 1)
            << "y lane " << i << " (" << Pairs[i * 2 + 1] << ")";
    }
}

TEST(PackHalf, RoundTripsWithinHalfPrecision)
{
    const std::vector<float> Pairs = MakePairs();
    const uint32 Count = (uint32)(Pairs.size() / 2);

    std::vector<uint32> Packed(Count, 0u);
    SIMD::PackHalf2x16Array(Pairs.data(), Packed.data(), Count);

    for (uint32 i = 0; i < Count; ++i)
    {
        const TVec<float, 2> Unpacked = Math::UnpackHalf2x16(Packed[i]);

        for (int32 Lane = 0; Lane < 2; ++Lane)
        {
            const float Source = Pairs[i * 2 + Lane];
            const float Result = Unpacked[Lane];

            // Half has 11 bits of mantissa; denormals below the half range collapse toward zero.
            const float Tolerance = Math::Max(Math::Abs(Source) * 0.001f, 7e-5f);
            EXPECT_LE(Math::Abs(Result - Source), Tolerance) << "lane " << Lane << " of pair " << i;
        }
    }
}

TEST(PackHalf, EmptyCountIsSafe)
{
    uint32 Sentinel = 0xDEADBEEFu;
    SIMD::PackHalf2x16Array(nullptr, &Sentinel, 0u);
    EXPECT_EQ(Sentinel, 0xDEADBEEFu);
}
