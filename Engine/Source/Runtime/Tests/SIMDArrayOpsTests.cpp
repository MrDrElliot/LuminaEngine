#include <gtest/gtest.h>

#include "Core/Math/SIMD/ArrayOps.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

#include <cmath>

using namespace Lumina;

namespace
{
    // Counts either side of the 8-wide step so the vector body and the scalar tail both run.
    constexpr int32 kCounts[] = { 0, 1, 7, 8, 9, 15, 16, 33, 257 };

    TVector<float> Ramp(int32 Count, float Seed)
    {
        TVector<float> Out((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Out[(size_t)i] = std::sin(Seed + (float)i * 0.37f) * 12.0f;
        }
        return Out;
    }

    // Doubles so the reference carries more precision than either float path it judges.
    void ExpectClose(const TVector<float>& Got, const TVector<double>& Want, const char* What)
    {
        ASSERT_EQ(Got.size(), Want.size()) << What;
        for (size_t i = 0; i < Got.size(); ++i)
        {
            const double Scale = std::fmax(1.0, std::fabs(Want[i]));
            EXPECT_NEAR((double)Got[i], Want[i], Scale * 1e-5) << What << " at lane " << i;
        }
    }
}

TEST(SIMDArrayOps, LerpArrayMatchesReference)
{
    for (int32 Count : kCounts)
    {
        const TVector<float> A = Ramp(Count, 0.0f);
        const TVector<float> B = Ramp(Count, 5.5f);
        constexpr float Alpha = 0.3125f;

        TVector<double> Want((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Want[(size_t)i] = (double)A[(size_t)i] * (1.0 - Alpha) + (double)B[(size_t)i] * Alpha;
        }

        TVector<float> Got((size_t)Count);
        SIMD::LerpArray(Got.data(), A.data(), B.data(), Count, Alpha);
        ExpectClose(Got, Want, "LerpArray");
    }
}

TEST(SIMDArrayOps, LerpArrayVarAlphaMatchesReference)
{
    for (int32 Count : kCounts)
    {
        const TVector<float> A = Ramp(Count, 1.0f);
        const TVector<float> B = Ramp(Count, 9.0f);

        TVector<float> Alphas((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Alphas[(size_t)i] = (float)(i % 9) / 8.0f;
        }

        TVector<double> Want((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            const double T = Alphas[(size_t)i];
            Want[(size_t)i] = (double)A[(size_t)i] * (1.0 - T) + (double)B[(size_t)i] * T;
        }

        TVector<float> Got((size_t)Count);
        SIMD::LerpArrayVarAlpha(Got.data(), A.data(), B.data(), Alphas.data(), Count);
        ExpectClose(Got, Want, "LerpArrayVarAlpha");
    }
}

TEST(SIMDArrayOps, AddScaledArrayMatchesReference)
{
    for (int32 Count : kCounts)
    {
        const TVector<float> A = Ramp(Count, 2.0f);
        const TVector<float> B = Ramp(Count, 7.25f);
        constexpr float S = -1.75f;

        TVector<double> Want((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Want[(size_t)i] = (double)A[(size_t)i] + (double)B[(size_t)i] * S;
        }

        TVector<float> Got((size_t)Count);
        SIMD::AddScaledArray(Got.data(), A.data(), B.data(), S, Count);
        ExpectClose(Got, Want, "AddScaledArray");
    }
}

TEST(SIMDArrayOps, MulAddArrayMatchesReference)
{
    for (int32 Count : kCounts)
    {
        const TVector<float> Base  = Ramp(Count, 3.0f);
        const TVector<float> Dir   = Ramp(Count, 4.5f);
        const TVector<float> Scale = Ramp(Count, 8.75f);

        TVector<double> Want((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Want[(size_t)i] = (double)Base[(size_t)i] + (double)Dir[(size_t)i] * (double)Scale[(size_t)i];
        }

        TVector<float> Got((size_t)Count);
        SIMD::MulAddArray(Got.data(), Base.data(), Dir.data(), Scale.data(), Count);
        ExpectClose(Got, Want, "MulAddArray");
    }
}

TEST(SIMDArrayOps, MulLerpOneArrayMatchesReference)
{
    for (int32 Count : kCounts)
    {
        const TVector<float> A = Ramp(Count, 6.0f);
        const TVector<float> B = Ramp(Count, 2.25f);
        constexpr float S = 0.625f;

        TVector<double> Want((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Want[(size_t)i] = (double)A[(size_t)i] * (1.0 + S * ((double)B[(size_t)i] - 1.0));
        }

        TVector<float> Got((size_t)Count);
        SIMD::MulLerpOneArray(Got.data(), A.data(), B.data(), S, Count);
        ExpectClose(Got, Want, "MulLerpOneArray");
    }
}

TEST(SIMDArrayOps, OutputMayAliasAnInput)
{
    constexpr int32 Count = 33;
    const TVector<float> A = Ramp(Count, 0.5f);
    const TVector<float> B = Ramp(Count, 3.5f);
    constexpr float Alpha = 0.25f;

    TVector<float> Separate((size_t)Count);
    SIMD::LerpArray(Separate.data(), A.data(), B.data(), Count, Alpha);

    TVector<float> InPlace = A;
    SIMD::LerpArray(InPlace.data(), InPlace.data(), B.data(), Count, Alpha);

    for (int32 i = 0; i < Count; ++i)
    {
        EXPECT_FLOAT_EQ(InPlace[(size_t)i], Separate[(size_t)i]) << "aliased write diverged at lane " << i;
    }
}
