#include <gtest/gtest.h>
#include "Containers/Algorithm.h"
#include "World/Entity/Components/ForestComponent.h"
#include "World/Forest/ForestScatter.h"

using namespace Lumina;

namespace
{
    Forest::FRules MakeRules(float Coverage)
    {
        Forest::FRules Rules;
        Rules.Seed     = 11;
        Rules.Center   = FVector3(0.0f);
        Rules.Extent   = 256.0f;
        Rules.TileSize = 32.0f;

        Forest::FSpeciesRules& Species = Rules.Species.EmplaceGetRef();
        Species.Density      = 0.05f;
        Species.ScaleMin     = 0.8f;
        Species.ScaleMax     = 1.2f;
        Species.MinSlopeCos  = 0.0f;
        Species.MinHeight    = -1000.0f;
        Species.MaxHeight    = 1000.0f;
        Species.ClusterScale = 40.0f;
        Species.Coverage     = Coverage;
        return Rules;
    }

    TVector<FVector3> SortedPositions(TVector<Forest::FPlant>& Plants)
    {
        TVector<FVector3> Out;
        for (const Forest::FPlant& Plant : Plants)
        {
            Out.Add(Plant.Position);
        }
        Algo::Sort(Out, [](const FVector3& A, const FVector3& B) { return A.x != B.x ? A.x < B.x : A.z < B.z; });
        return Out;
    }

    struct FPaintedChunk
    {
        int32 X      = 0;
        int32 Z      = 0;
        uint8 Weight = SForestPaintChunk::kNeutral;
    };

    TSharedPtr<const Forest::FPaint> MakePaint(std::initializer_list<FPaintedChunk> Chunks, std::initializer_list<FVector3> Placed = {})
    {
        SForestComponent Forest;
        Forest.Species.SetNum(1);
        for (const FPaintedChunk& Painted : Chunks)
        {
            SForestPaintChunk& Chunk = Forest.FindOrAddPaint(Painted.X, Painted.Z, 0);
            Chunk.Weights.SetNum(SForestPaintChunk::kCells * SForestPaintChunk::kCells, Painted.Weight);
            for (uint8& Weight : Chunk.Weights)
            {
                Weight = Painted.Weight;
            }
        }
        for (const FVector3& Position : Placed)
        {
            SFoliageInstance& Plant = Forest.Placed.EmplaceGetRef();
            Plant.Position = Position;
        }
        TSharedPtr<Forest::FPaint> Paint = MakeShared<Forest::FPaint>();
        Forest::CapturePaint(Forest, FVector3(0.0f), *Paint);
        return Paint;
    }

    int32 CountPlants(const Forest::FRules& Rules, const FVector2& Min, const FVector2& Max)
    {
        TVector<Forest::FPlant> Plants;
        Forest::ScatterRect(Rules, Min, Max, Plants);
        return Plants.Num();
    }

}

TEST(ForestScatter, SplittingARegionScattersTheSamePlants)
{
    const Forest::FRules Rules = MakeRules(0.6f);

    TVector<Forest::FPlant> Whole;
    Forest::ScatterRect(Rules, FVector2(-64.0f), FVector2(64.0f), Whole);

    TVector<Forest::FPlant> Quarters;
    for (int32 Quarter = 0; Quarter < 4; ++Quarter)
    {
        const FVector2 Min(-64.0f + 64.0f * (float)(Quarter & 1), -64.0f + 64.0f * (float)(Quarter >> 1));
        Forest::ScatterRect(Rules, Min, Min + FVector2(64.0f), Quarters);
    }

    ASSERT_FALSE(Whole.IsEmpty());
    const TVector<FVector3> A = SortedPositions(Whole);
    const TVector<FVector3> B = SortedPositions(Quarters);
    ASSERT_EQ(A.Num(), B.Num());
    for (int32 i = 0; i < A.Num(); ++i)
    {
        EXPECT_EQ(A[i], B[i]);
    }
}

TEST(ForestScatter, CoverageThinsTheStandsAndZeroLeavesBareGround)
{
    TVector<Forest::FPlant> Full;
    Forest::ScatterRect(MakeRules(1.0f), FVector2(-128.0f), FVector2(128.0f), Full);
    TVector<Forest::FPlant> Half;
    Forest::ScatterRect(MakeRules(0.5f), FVector2(-128.0f), FVector2(128.0f), Half);
    TVector<Forest::FPlant> None;
    Forest::ScatterRect(MakeRules(0.0f), FVector2(-128.0f), FVector2(128.0f), None);

    // One candidate per cell, so a full spread holds about density times area.
    EXPECT_NEAR((float)Full.Num(), 0.05f * 256.0f * 256.0f, 0.05f * 256.0f * 256.0f * 0.1f);
    EXPECT_LT(Half.Num(), Full.Num());
    EXPECT_GT(Half.Num(), 0);
    EXPECT_TRUE(None.IsEmpty());
}

TEST(ForestScatter, RegionsOutsideTheForestAreEmpty)
{
    TVector<Forest::FPlant> Plants;
    Forest::ScatterRect(MakeRules(1.0f), FVector2(200.0f), FVector2(300.0f), Plants);
    EXPECT_TRUE(Plants.IsEmpty());
}

TEST(ForestScatter, PlantsStandOnTheFinestTerrainUnderThem)
{
    auto MakeFlat = [](float Size, float Height)
    {
        const float Heights[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        FTerrainHeightView View;
        View.Heights       = Heights;
        View.Resolution    = 2;
        View.TileWorldSize = Size;
        View.MaxHeight     = Height;
        return TSharedPtr<const Forest::FTerrainSnapshot>(Forest::FTerrainSnapshot::Capture(View));
    };

    Forest::FRules Rules = MakeRules(1.0f);
    Rules.Terrains = { MakeFlat(64.0f, 10.0f), MakeFlat(512.0f, 3.0f) };

    TVector<Forest::FPlant> Plants;
    Forest::ScatterRect(Rules, FVector2(-128.0f), FVector2(128.0f), Plants);

    int32 OnFine = 0;
    int32 OnBackdrop = 0;
    for (const Forest::FPlant& Plant : Plants)
    {
        const bool bInsideFine = Math::Abs(Plant.Position.x) < 32.0f && Math::Abs(Plant.Position.z) < 32.0f;
        EXPECT_FLOAT_EQ(Plant.Position.y, bInsideFine ? 10.0f : 3.0f);
        (bInsideFine ? OnFine : OnBackdrop)++;
    }
    EXPECT_GT(OnFine, 0);
    EXPECT_GT(OnBackdrop, 0);
}

TEST(ForestPaint, PaintClearsOrFillsOnlyTheChunkUnderIt)
{
    Forest::FRules Rules = MakeRules(0.5f);
    const FVector2 Chunk(0.0f), ChunkEnd(SForestPaintChunk::kSize);
    const FVector2 Neighbor(-SForestPaintChunk::kSize, 0.0f), NeighborEnd(0.0f, SForestPaintChunk::kSize);
    const int32 Before = CountPlants(Rules, Chunk, ChunkEnd);
    const int32 NeighborBefore = CountPlants(Rules, Neighbor, NeighborEnd);
    ASSERT_GT(Before, 0);

    Rules.Paint = MakePaint({ { 0, 0, 0 } });
    EXPECT_EQ(CountPlants(Rules, Chunk, ChunkEnd), 0);
    EXPECT_EQ(CountPlants(Rules, Neighbor, NeighborEnd), NeighborBefore);

    Rules.Paint = MakePaint({ { 0, 0, 255 } });
    EXPECT_GT(CountPlants(Rules, Chunk, ChunkEnd), Before);
    EXPECT_EQ(CountPlants(Rules, Neighbor, NeighborEnd), NeighborBefore);
}

TEST(ForestPaint, ASpeciesTheRulesLeaveOutGrowsWhereItIsPainted)
{
    Forest::FRules Rules = MakeRules(0.0f);
    Rules.Paint = MakePaint({ { 0, 0, 255 } });

    TVector<Forest::FPlant> Plants;
    Forest::ScatterRect(Rules, FVector2(-128.0f), FVector2(128.0f), Plants);
    ASSERT_FALSE(Plants.IsEmpty());
    // Jitter may carry a plant up to half a cell past the chunk whose cell it grew in.
    const float Margin = 0.5f / Math::Sqrt(Rules.Species[0].Density);
    for (const Forest::FPlant& Plant : Plants)
    {
        EXPECT_GE(Plant.Position.x, -Margin);
        EXPECT_LT(Plant.Position.x, SForestPaintChunk::kSize + Margin);
        EXPECT_GE(Plant.Position.z, -Margin);
        EXPECT_LT(Plant.Position.z, SForestPaintChunk::kSize + Margin);
    }

    // Split along lines that cut through the painted chunk, which must not change what grows.
    TVector<Forest::FPlant> Quarters;
    for (int32 Quarter = 0; Quarter < 4; ++Quarter)
    {
        const FVector2 Min(-128.0f + 150.0f * (float)(Quarter & 1), -128.0f + 150.0f * (float)(Quarter >> 1));
        const FVector2 Max(Quarter & 1 ? 128.0f : 22.0f, Quarter >> 1 ? 128.0f : 22.0f);
        Forest::ScatterRect(Rules, Min, Max, Quarters);
    }
    const TVector<FVector3> A = SortedPositions(Plants);
    const TVector<FVector3> B = SortedPositions(Quarters);
    ASSERT_EQ(A.Num(), B.Num());
    for (int32 i = 0; i < A.Num(); ++i)
    {
        EXPECT_EQ(A[i], B[i]);
    }
}

TEST(ForestPaint, PlacedPlantsShowInTheTileThatHoldsThem)
{
    Forest::FRules Rules = MakeRules(0.0f);
    Rules.Paint = MakePaint({}, { FVector3(10.0f, 5.0f, 20.0f) });

    TVector<Forest::FPlant> Plants;
    Forest::ScatterRect(Rules, FVector2(0.0f), FVector2(32.0f), Plants);
    ASSERT_EQ(Plants.Num(), 1);
    EXPECT_EQ(Plants[0].Position, FVector3(10.0f, 5.0f, 20.0f));
    EXPECT_EQ(CountPlants(Rules, FVector2(32.0f, 0.0f), FVector2(64.0f, 32.0f)), 0);
}

TEST(ForestPaint, OnlyTheChunkWhosePaintChangedIsDiffed)
{
    Forest::FRules Before = MakeRules(0.5f);
    Before.Paint = MakePaint({ { 0, 0, 0 } });
    Forest::FRules After = MakeRules(0.5f);
    After.Paint = MakePaint({ { 0, 0, 0 }, { 1, 0, 255 } });

    const float Size = SForestPaintChunk::kSize;
    TVector<Forest::FArea> Changed;
    Forest::DiffPaint(After, Before.Paint.get(), After.Paint.get(), Changed);
    ASSERT_EQ(Changed.Num(), 1);
    EXPECT_EQ(Changed[0].Min, FVector2(Size, 0.0f));
    EXPECT_EQ(Changed[0].Max, FVector2(Size * 2.0f, Size));
}
