#include <gtest/gtest.h>

#include "Core/Math/Math.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Subsystems/TerrainSculptSystem.h"

#include <cmath>

using namespace Lumina;

namespace
{
    constexpr int32 kRes       = 65;
    constexpr float kTileSize  = 256.0f;
    constexpr float kMaxHeight = 64.0f;
    constexpr float kStride    = kTileSize / float(kRes - 1);

    template<typename THeight>
    STerrainComponent MakeTerrain(THeight&& Height)
    {
        STerrainComponent Terrain;
        Terrain.Resolution    = kRes;
        Terrain.TileWorldSize = kTileSize;
        Terrain.MaxHeight     = kMaxHeight;
        TVector<float> Heights(kRes * kRes);
        for (int32 Y = 0; Y < kRes; ++Y)
        {
            for (int32 X = 0; X < kRes; ++X)
            {
                Heights[Y * kRes + X] = Height(X, Y);
            }
        }
        Terrain.SetStore(FTerrainStore::CreateFromArrays(kRes, Heights, {}, 0));
        return Terrain;
    }

    // World position of sample (X, Y) on a terrain centered on the origin.
    FVector3 SamplePosition(int32 X, int32 Y)
    {
        return FVector3(float(X) * kStride - kTileSize * 0.5f, 0.0f, float(Y) * kStride - kTileSize * 0.5f);
    }

    FTerrainSculptDab MakeDab(ETerrainBrushMode Mode, float Radius, float Strength)
    {
        FTerrainSculptDab Dab;
        Dab.Mode          = Mode;
        Dab.WorldPosition = SamplePosition(kRes / 2, kRes / 2);
        Dab.Radius        = Radius;
        Dab.Strength      = Strength;
        return Dab;
    }

    void Hold(STerrainComponent& Terrain, FTerrainSculptDab Dab, float Seconds, float FramesPerSecond = 60.0f)
    {
        Dab.DeltaSeconds = 1.0f / FramesPerSecond;
        const int32 Frames = int32(Seconds * FramesPerSecond + 0.5f);
        for (int32 Frame = 0; Frame < Frames; ++Frame)
        {
            FTerrainSculptSystem::ApplyDab(Terrain, Dab);
        }
    }

    float At(const STerrainComponent& Terrain, int32 X, int32 Y)
    {
        return Terrain.GetStore()->GetHeight(X, Y);
    }

    TVector<float> ReadAll(const STerrainComponent& Terrain)
    {
        TVector<float> Heights(kRes * kRes);
        Terrain.GetStore()->ReadHeights(0, FIntVector2(0), FIntVector2(kRes), Heights.GetData());
        return Heights;
    }

    // Spread of the heights in a square around the center, which smoothing should shrink.
    float CenterVariance(const STerrainComponent& Terrain, int32 HalfSize)
    {
        double Sum = 0.0;
        double SumSq = 0.0;
        int32 Count = 0;
        for (int32 Y = kRes / 2 - HalfSize; Y <= kRes / 2 + HalfSize; ++Y)
        {
            for (int32 X = kRes / 2 - HalfSize; X <= kRes / 2 + HalfSize; ++X)
            {
                const double H = At(Terrain, X, Y);
                Sum += H;
                SumSq += H * H;
                ++Count;
            }
        }
        const double Mean = Sum / Count;
        return float(SumSq / Count - Mean * Mean);
    }

    double TotalHeight(const STerrainComponent& Terrain)
    {
        double Sum = 0.0;
        for (float H : ReadAll(Terrain))
        {
            Sum += H;
        }
        return Sum;
    }
}

TEST(TerrainSculpt, FalloffIsFullInTheCoreAndZeroAtTheRim)
{
    for (int32 Shape = 0; Shape < int32(ETerrainBrushFalloff::Num); ++Shape)
    {
        const ETerrainBrushFalloff Falloff = ETerrainBrushFalloff(Shape);
        EXPECT_FLOAT_EQ(FTerrainSculptSystem::FalloffWeight(0.0f, 10.0f, 0.5f, Falloff), 1.0f);
        EXPECT_FLOAT_EQ(FTerrainSculptSystem::FalloffWeight(4.9f, 10.0f, 0.5f, Falloff), 1.0f);
        EXPECT_FLOAT_EQ(FTerrainSculptSystem::FalloffWeight(10.0f, 10.0f, 0.5f, Falloff), 0.0f);

        float Previous = 1.0f;
        for (float Distance = 5.0f; Distance < 10.0f; Distance += 0.25f)
        {
            const float W = FTerrainSculptSystem::FalloffWeight(Distance, 10.0f, 0.5f, Falloff);
            EXPECT_LE(W, Previous + 1e-6f) << "shape " << Shape << " at " << Distance;
            Previous = W;
        }
    }

    // No falloff is a hard disk.
    EXPECT_FLOAT_EQ(FTerrainSculptSystem::FalloffWeight(9.99f, 10.0f, 0.0f, ETerrainBrushFalloff::Smooth), 1.0f);
}

TEST(TerrainSculpt, SculptRaisesAtTheStrengthScaledRate)
{
    STerrainComponent Terrain = MakeTerrain([](int32, int32) { return 0.0f; });
    const float Radius = 32.0f;
    Hold(Terrain, MakeDab(ETerrainBrushMode::Sculpt, Radius, 0.5f), 1.0f);

    // Half strength for a second moves the center by half the brush radius, which is under the height range here.
    EXPECT_NEAR(At(Terrain, kRes / 2, kRes / 2) * kMaxHeight, 0.5f * Radius, 0.5f);
    EXPECT_FLOAT_EQ(At(Terrain, 0, 0), 0.0f);
}

TEST(TerrainSculpt, BlendModesReachTheSameHeightAtAnyFrameRate)
{
    auto FlattenFor = [](float FramesPerSecond)
    {
        STerrainComponent Terrain = MakeTerrain([](int32, int32) { return 0.5f; });
        FTerrainSculptDab Dab = MakeDab(ETerrainBrushMode::Flatten, 40.0f, 0.2f);
        Dab.FlattenHeight = 0.0f;
        Hold(Terrain, Dab, 1.0f, FramesPerSecond);
        return At(Terrain, kRes / 2, kRes / 2);
    };

    const float At60 = FlattenFor(60.0f);
    EXPECT_NEAR(At60, FlattenFor(10.0f), 1e-3f);
    EXPECT_LT(At60, 0.5f);
    EXPECT_GT(At60, 0.0f);
}

TEST(TerrainSculpt, RaiseOnlyFlattenNeverLowersTheGround)
{
    STerrainComponent Terrain = MakeTerrain([](int32 X, int32 Y) { return 0.3f + 0.2f * std::sin(float(X) * 0.7f) * std::cos(float(Y) * 0.5f); });
    const TVector<float> Before = ReadAll(Terrain);

    FTerrainSculptDab Dab = MakeDab(ETerrainBrushMode::Flatten, 60.0f, 1.0f);
    Dab.FlattenHeight = 0.3f * kMaxHeight;
    Dab.FlattenSide   = ETerrainFlattenSide::RaiseOnly;
    Hold(Terrain, Dab, 2.0f);

    const TVector<float> After = ReadAll(Terrain);
    for (int32 I = 0; I < kRes * kRes; ++I)
    {
        EXPECT_GE(After[I], Before[I] - 1e-6f);
    }
    EXPECT_NEAR(At(Terrain, kRes / 2, kRes / 2), Math::Max(Before[(kRes / 2) * kRes + kRes / 2], 0.3f), 1e-3f);
}

TEST(TerrainSculpt, WiderSmoothBrushRelaxesWiderFeatures)
{
    // Ridges four samples apart, which a single-sample blur barely touches.
    auto Ridged = [](int32 X, int32) { return 0.5f + 0.2f * std::sin(float(X) * 1.5707963f * 0.5f); };

    STerrainComponent Narrow = MakeTerrain(Ridged);
    STerrainComponent Wide   = MakeTerrain(Ridged);
    const float Original = CenterVariance(Narrow, 4);

    FTerrainSculptDab Dab = MakeDab(ETerrainBrushMode::Smooth, 8.0f, 1.0f);
    Dab.Falloff = 0.0f;
    Hold(Narrow, Dab, 0.25f);

    Dab.Radius = 100.0f;
    Hold(Wide, Dab, 0.25f);

    EXPECT_LT(CenterVariance(Wide, 4), CenterVariance(Narrow, 4) * 0.5f);
    EXPECT_LT(CenterVariance(Wide, 4), Original * 0.25f);
}

TEST(TerrainSculpt, ErodeSlumpsACliffToTheTalusAngleAndKeepsItsMass)
{
    // A cliff a quarter of the height range tall across one sample, far past any talus.
    STerrainComponent Terrain = MakeTerrain([](int32 X, int32) { return X < kRes / 2 ? 0.25f : 0.0f; });
    const double MassBefore = TotalHeight(Terrain);

    FTerrainSculptDab Dab = MakeDab(ETerrainBrushMode::Erode, 120.0f, 1.0f);
    Dab.TalusAngle = 30.0f;
    Dab.Falloff    = 0.2f;
    Hold(Terrain, Dab, 4.0f);

    const float TalusDrop = std::tan(Math::Radians(30.0f)) * kStride / kMaxHeight;
    float Steepest = 0.0f;
    for (int32 X = kRes / 2 - 8; X < kRes / 2 + 8; ++X)
    {
        Steepest = Math::Max(Steepest, Math::Abs(At(Terrain, X + 1, kRes / 2) - At(Terrain, X, kRes / 2)));
    }

    EXPECT_LT(Steepest, TalusDrop * 1.5f);
    EXPECT_NEAR(TotalHeight(Terrain), MassBefore, MassBefore * 1e-4);
}

TEST(TerrainSculpt, TerraceGathersHeightsOntoTreads)
{
    STerrainComponent Terrain = MakeTerrain([](int32 X, int32) { return float(X) / float(kRes - 1) * 0.8f; });

    FTerrainSculptDab Dab = MakeDab(ETerrainBrushMode::Terrace, 400.0f, 1.0f);
    Dab.TerraceStep = 8.0f;
    Dab.Falloff     = 0.0f;
    Hold(Terrain, Dab, 3.0f);

    // A ramp spreads evenly over each step, so most samples ending near a tread means the risers sharpened.
    const float Step = Dab.TerraceStep / kMaxHeight;
    int32 OnTread = 0;
    for (int32 X = 0; X < kRes; ++X)
    {
        const float Level = At(Terrain, X, kRes / 2) / Step;
        if (Math::Abs(Level - std::round(Level)) < 0.15f)
        {
            ++OnTread;
        }
    }
    EXPECT_GT(OnTread, kRes * 3 / 4);
}
