#include <gtest/gtest.h>

#include "World/Scene/RenderScene/SunShadowMath.h"

#include <random>

using namespace Lumina;

TEST(SunShadowMath, ExponentOneSplitsEvenly)
{
    float Splits[NumCascades];
    SunShadow::ComputeCascadeSplits(1.0f, 201.0f, 4, 1.0f, false, 0.0f, Splits);
    EXPECT_NEAR(Splits[0], 51.0f, 1e-3f);
    EXPECT_NEAR(Splits[1], 101.0f, 1e-3f);
    EXPECT_NEAR(Splits[2], 151.0f, 1e-3f);
    EXPECT_NEAR(Splits[3], 201.0f, 1e-3f);
}

TEST(SunShadowMath, EachCascadeSpansExponentTimesThePrevious)
{
    float Splits[NumCascades];
    SunShadow::ComputeCascadeSplits(1.0f, 200.0f, 3, 3.0f, false, 0.0f, Splits);
    const float First  = Splits[0] - 1.0f;
    const float Second = Splits[1] - Splits[0];
    const float Third  = Splits[2] - Splits[1];
    EXPECT_NEAR(Second / First, 3.0f, 1e-3f);
    EXPECT_NEAR(Third / Second, 3.0f, 1e-3f);
    EXPECT_NEAR(Splits[2], 200.0f, 1e-3f);
}

TEST(SunShadowMath, UnusedCascadesSitAtTheFarEnd)
{
    float Splits[NumCascades];
    SunShadow::ComputeCascadeSplits(1.0f, 200.0f, 2, 3.0f, false, 0.0f, Splits);
    EXPECT_NEAR(Splits[1], 200.0f, 1e-3f);
    EXPECT_NEAR(Splits[2], 200.0f, 1e-3f);
    EXPECT_NEAR(Splits[3], 200.0f, 1e-3f);
}

TEST(SunShadowMath, FarCascadeFollowsTheNearOnes)
{
    float Splits[NumCascades];
    SunShadow::ComputeCascadeSplits(1.0f, 200.0f, 3, 3.0f, true, 3000.0f, Splits);
    EXPECT_NEAR(Splits[2], 200.0f, 1e-3f);
    EXPECT_NEAR(Splits[3], 3000.0f, 1e-3f);
    for (int32 i = 1; i < NumCascades; ++i)
    {
        EXPECT_GT(Splits[i], Splits[i - 1]);
    }
}

TEST(SunShadowMath, GridBasisIsOrthonormalAndFacesTheSun)
{
    for (const FVector3 ToSun : { FVector3(0.9f, 0.45f, -0.35f), FVector3(0.0f, 1.0f, 0.0f), FVector3(0.01f, -0.999f, 0.0f) })
    {
        const FVector3 Dir = Math::Normalize(ToSun);
        const FVector3 Points[] = { FVector3(0.0f), FVector3(10.0f, 5.0f, -3.0f) };
        const SunShadow::FSunGrid Grid = SunShadow::FitSunGrid(Dir, Points, 16u);
        EXPECT_NEAR(Math::Length(Grid.Right), 1.0f, 1e-4f);
        EXPECT_NEAR(Math::Length(Grid.Up), 1.0f, 1e-4f);
        EXPECT_NEAR(Math::Dot(Grid.Right, Grid.Up), 0.0f, 1e-4f);
        EXPECT_NEAR(Math::Dot(Grid.Right, Dir), 0.0f, 1e-4f);
        EXPECT_NEAR(Math::Dot(Grid.Up, Dir), 0.0f, 1e-4f);
    }
}

TEST(SunShadowMath, EveryFittedPointLandsInsideTheGrid)
{
    constexpr uint32 Dimension = 128u;
    std::mt19937 Rng(7u);
    std::uniform_real_distribution<float> Coord(-900.0f, 900.0f);

    TVector<FVector3> Points(512);
    for (FVector3& Point : Points)
    {
        Point = FVector3(Coord(Rng), Coord(Rng) * 0.2f, Coord(Rng));
    }

    const FVector3 ToSun = Math::Normalize(FVector3(0.9f, 0.45f, -0.35f));
    const SunShadow::FSunGrid Grid = SunShadow::FitSunGrid(ToSun, Points, Dimension);
    for (const FVector3& Point : Points)
    {
        const FIntVector2 Cell = Grid.CellOf(Point);
        EXPECT_GE(Cell.x, 0);
        EXPECT_GE(Cell.y, 0);
        EXPECT_LT(Cell.x, (int32)Dimension);
        EXPECT_LT(Cell.y, (int32)Dimension);
    }
}

TEST(SunShadowMath, ARayTowardTheSunStaysInItsCell)
{
    const FVector3 ToSun = Math::Normalize(FVector3(0.9f, 0.45f, -0.35f));
    const FVector3 Corners[] = { FVector3(-500.0f, 0.0f, -500.0f), FVector3(500.0f, 300.0f, 500.0f) };
    const SunShadow::FSunGrid Grid = SunShadow::FitSunGrid(ToSun, Corners, 64u);

    std::mt19937 Rng(11u);
    std::uniform_real_distribution<float> Coord(-400.0f, 400.0f);
    std::uniform_real_distribution<float> Travel(0.0f, 1500.0f);
    for (int32 i = 0; i < 256; ++i)
    {
        const FVector3 Receiver(Coord(Rng), Coord(Rng) * 0.3f, Coord(Rng));
        const FIntVector2 Start = Grid.CellOf(Receiver);
        const FIntVector2 Along = Grid.CellOf(Receiver + ToSun * Travel(Rng));

        // Equal up to the rounding of the two projections at a cell boundary.
        EXPECT_LE(Math::Abs(Start.x - Along.x), 1);
        EXPECT_LE(Math::Abs(Start.y - Along.y), 1);
    }
}
