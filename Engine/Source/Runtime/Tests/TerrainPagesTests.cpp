#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Scene/RenderScene/TerrainPages.h"

using namespace Lumina;

namespace
{
    STerrainComponent MakeTerrain(int32 Resolution, TSpan<const uint8> LayerWeights = {}, int32 LayerCount = 0)
    {
        STerrainComponent Terrain;
        Terrain.Resolution    = Resolution;
        Terrain.TileWorldSize = float(Resolution - 1);
        Terrain.MaxHeight     = 100.0f;
        Terrain.Layers.SetNum(LayerCount);
        TVector<float> Heights(Resolution * Resolution);
        for (int32 Y = 0; Y < Resolution; ++Y)
        {
            for (int32 X = 0; X < Resolution; ++X)
            {
                Heights[Y * Resolution + X] = float((X * 7 + Y * 13) % 101) / 100.0f;
            }
        }
        Terrain.SetStore(FTerrainStore::CreateFromArrays(Resolution, Heights, LayerWeights, LayerCount));
        return Terrain;
    }

    FTerrainPageLayout MakeLayout(const STerrainComponent& Terrain)
    {
        FTerrainPageLayout Layout;
        Layout.OriginXZ      = FVector2(0.0f);
        Layout.SampleSpacing = Terrain.TileWorldSize / float(Terrain.Resolution - 1);
        Layout.MaxHeight     = Terrain.MaxHeight;
        Layout.Resolution    = Terrain.Resolution;
        return Layout;
    }

    // A 1080p view with a 50 degree vertical field, the editor's default.
    FTerrainPageView MakeView(const FVector3& Camera)
    {
        FTerrainPageView View;
        View.Camera        = Camera;
        View.PixelsPerUnit = 0.5f * 1080.0f / Math::Tan(Math::Radians(25.0f));
        return View;
    }

    int32 CountMip(const TVector<FTerrainPageRequest>& Wanted, int32 Mip)
    {
        int32 Count = 0;
        for (const FTerrainPageRequest& Request : Wanted)
        {
            Count += Request.Mip == Mip ? 1 : 0;
        }
        return Count;
    }

    ECS::FEntity MakeEntity(uint32 Index)
    {
        return ECS::FEntity(Index);
    }
}

TEST(TerrainPages, RootIsWantedFromAnyDistance)
{
    STerrainComponent Terrain = MakeTerrain(1025);
    TVector<FTerrainPageRequest> Wanted;
    TerrainPages::SelectWanted(MakeLayout(Terrain), Terrain.GetStore()->GetBounds(), MakeView(FVector3(1.0e7f, 0.0f, 1.0e7f)), Wanted);
    ASSERT_EQ(Wanted.Num(), 1);
    EXPECT_EQ(Wanted[0].Node, 0u);
    EXPECT_EQ(Wanted[0].Mip, GetTerrainPageDepth(GetTerrainTreeDepth(1025)));
}

TEST(TerrainPages, FinePagesGatherAroundTheCamera)
{
    STerrainComponent Terrain = MakeTerrain(4097);
    TVector<FTerrainPageRequest> Wanted;
    TerrainPages::SelectWanted(MakeLayout(Terrain), Terrain.GetStore()->GetBounds(), MakeView(FVector3(100.0f, 150.0f, 100.0f)), Wanted);

    // Mip 0 pages stay within a few hundred meters of a 1 m spacing camera, and every one of them is near it.
    const int32 Fine = CountMip(Wanted, 0);
    EXPECT_GT(Fine, 0);
    EXPECT_LT(Fine, 64 * 64);
    for (const FTerrainPageRequest& Request : Wanted)
    {
        if (Request.Mip == 0)
        {
            EXPECT_LT(Request.Distance, 2000.0f);
        }
    }
}

TEST(TerrainPages, ParentsComeBeforeChildren)
{
    STerrainComponent Terrain = MakeTerrain(2049);
    TVector<FTerrainPageRequest> Wanted;
    TerrainPages::SelectWanted(MakeLayout(Terrain), Terrain.GetStore()->GetBounds(), MakeView(FVector3(1000.0f, 50.0f, 1000.0f)), Wanted);
    for (int32 Index = 1; Index < Wanted.Num(); ++Index)
    {
        EXPECT_GE(Wanted[Index - 1].Mip, Wanted[Index].Mip);
    }
}

TEST(TerrainPages, BuildClampsTheBorderAtTheMapEdge)
{
    TVector<uint8> LayerWeights(2 * 129 * 129, uint8(0));
    LayerWeights[129 * 129 + 0] = 200u;
    STerrainComponent Terrain = MakeTerrain(129, LayerWeights, 2);

    TVector<uint16> Heights(GTerrainPageTexelCount);
    TVector<uint16> Normals(GTerrainPageTexelCount);
    TVector<uint32> Weights(GTerrainPageTexelCount);
    // Depth 1 holds mip 0 pages for a 129 map, and node 1 is the page at the origin.
    TerrainPages::Build(*Terrain.GetStore(), MakeLayout(Terrain), 1u, Heights.GetData(), Normals.GetData(), Weights.GetData());

    const auto Encoded = [&](int32 X, int32 Y) { return (uint16)(Math::Clamp(Terrain.GetStore()->GetHeight(X, Y), 0.0f, 1.0f) * 65535.0f + 0.5f); };
    // The gutter before the origin repeats sample 0, and texel Gutter is sample 0 itself.
    EXPECT_EQ(Heights[0], Encoded(0, 0));
    EXPECT_EQ(Heights[GTerrainPageGutter * GTerrainPageTexels + GTerrainPageGutter], Encoded(0, 0));
    EXPECT_EQ(Heights[(GTerrainPageGutter + 3) * GTerrainPageTexels + GTerrainPageGutter + 5], Encoded(5, 3));
    // Layer 1 lands in the second byte.
    EXPECT_EQ((Weights[GTerrainPageGutter * GTerrainPageTexels + GTerrainPageGutter] >> 8) & 0xFFu, 200u);
}

TEST(TerrainPages, StreamerUploadsRootFirstAndFillsThePageTable)
{
    STerrainComponent Terrain = MakeTerrain(1025);
    const ECS::FEntity Entity = MakeEntity(1);

    FTerrainPageStreamer Streamer;
    Streamer.BeginFrame();
    FTerrainPageTable& Table = Streamer.Prepare(Entity, Terrain.Resolution, Terrain.MaxHeight, Terrain.TileWorldSize);
    Streamer.Request(Entity, Terrain.Store, MakeLayout(Terrain), MakeView(FVector3(512.0f, 120.0f, 512.0f)));
    Streamer.Commit();

    ASSERT_FALSE(Streamer.UploadSlots.IsEmpty());
    EXPECT_EQ(Table.Slots[0], Streamer.UploadSlots[0]);
    EXPECT_EQ(Streamer.UploadHeights.Num(), Streamer.UploadSlots.Num() * GTerrainPageTexelCount);
    EXPECT_LE((uint32)Streamer.UploadSlots.Num(), GTerrainPageUploadsPerFrame);
    EXPECT_TRUE(Table.bReset);
}

TEST(TerrainPages, StalePagesUploadAgainInTheirSlot)
{
    STerrainComponent Terrain = MakeTerrain(257);
    const ECS::FEntity Entity = MakeEntity(1);
    const FTerrainPageView View = MakeView(FVector3(10.0f, 80.0f, 10.0f));

    FTerrainPageStreamer Streamer;
    for (int32 Frame = 0; Frame < 4; ++Frame)
    {
        Streamer.BeginFrame();
        Streamer.Prepare(Entity, Terrain.Resolution, Terrain.MaxHeight, Terrain.TileWorldSize);
        Streamer.Request(Entity, Terrain.Store, MakeLayout(Terrain), View);
        Streamer.Commit();
        Streamer.UploadSlots.Reset();
        Streamer.UploadHeights.Reset();
        Streamer.UploadWeights.Reset();
    }

    const uint32 RootSlot = Streamer.FindTable(Entity)->Slots[0];
    Streamer.MarkStale(Entity, FIntVector2(3, 3), FIntVector2(3, 3));
    Streamer.BeginFrame();
    Streamer.Prepare(Entity, Terrain.Resolution, Terrain.MaxHeight, Terrain.TileWorldSize);
    Streamer.Request(Entity, Terrain.Store, MakeLayout(Terrain), View);
    Streamer.Commit();

    // Every mip has a page over the edit, so each refreshes in place and nothing else uploads.
    EXPECT_TRUE(Algo::Contains(Streamer.UploadSlots, RootSlot));
    EXPECT_EQ(Streamer.FindTable(Entity)->Slots[0], RootSlot);
    EXPECT_LE(Streamer.UploadSlots.Num(), GetTerrainPageDepth(GetTerrainTreeDepth(257)) + 1);
}

TEST(TerrainPages, ResolutionChangeDropsEveryPage)
{
    STerrainComponent Terrain = MakeTerrain(257);
    const ECS::FEntity Entity = MakeEntity(1);

    FTerrainPageStreamer Streamer;
    Streamer.BeginFrame();
    Streamer.Prepare(Entity, Terrain.Resolution, Terrain.MaxHeight, Terrain.TileWorldSize);
    Streamer.Request(Entity, Terrain.Store, MakeLayout(Terrain), MakeView(FVector3(0.0f, 50.0f, 0.0f)));
    Streamer.Commit();
    Streamer.FindTable(Entity)->bReset = false;

    FTerrainPageTable& Table = Streamer.Prepare(Entity, 513, Terrain.MaxHeight, Terrain.TileWorldSize);
    EXPECT_TRUE(Table.bReset);
    EXPECT_EQ((uint32)Table.Slots.Num(), GetTerrainNodeCount(GetTerrainPageDepth(GetTerrainTreeDepth(513))));
    EXPECT_TRUE(Algo::AllOf(Table.Slots, [](uint32 Slot) { return Slot == GTerrainPageNone; }));
}

TEST(TerrainPages, PrunedTerrainsGiveTheirSlotsBack)
{
    STerrainComponent Terrain = MakeTerrain(257);
    const ECS::FEntity Gone = MakeEntity(1);
    const ECS::FEntity Kept = MakeEntity(2);

    FTerrainPageStreamer Streamer;
    Streamer.BeginFrame();
    Streamer.Prepare(Gone, Terrain.Resolution, Terrain.MaxHeight, Terrain.TileWorldSize);
    Streamer.Request(Gone, Terrain.Store, MakeLayout(Terrain), MakeView(FVector3(0.0f, 50.0f, 0.0f)));
    Streamer.Commit();
    const uint32 Used = Streamer.FindTable(Gone)->Slots[0];

    Streamer.BeginFrame();
    Streamer.Prune({ Kept });
    EXPECT_EQ(Streamer.FindTable(Gone), nullptr);

    Streamer.UploadSlots.Reset();
    Streamer.Prepare(Kept, Terrain.Resolution, Terrain.MaxHeight, Terrain.TileWorldSize);
    Streamer.Request(Kept, Terrain.Store, MakeLayout(Terrain), MakeView(FVector3(0.0f, 50.0f, 0.0f)));
    Streamer.Commit();
    EXPECT_TRUE(Algo::Contains(Streamer.UploadSlots, Used));
}

TEST(TerrainPages, FlatGroundPacksAnUpNormal)
{
    STerrainComponent Terrain = MakeTerrain(129);
    Terrain.SetStore(FTerrainStore::CreateFlat(129, 0.5f));

    TVector<uint16> Heights(GTerrainPageTexelCount);
    TVector<uint16> Normals(GTerrainPageTexelCount);
    TVector<uint32> Weights(GTerrainPageTexelCount);
    TerrainPages::Build(*Terrain.GetStore(), MakeLayout(Terrain), 1u, Heights.GetData(), Normals.GetData(), Weights.GetData());

    // Zero slope packs to the middle of both bytes.
    EXPECT_EQ(Normals[GTerrainPageGutter * GTerrainPageTexels + GTerrainPageGutter], uint16(128u | (128u << 8)));
}

TEST(TerrainPages, CoarseWeightsAverageTheirFootprint)
{
    // A one-sample stripe that decimation at a four-sample step would either catch whole or miss.
    TVector<uint8> LayerWeights(1025 * 1025, uint8(0));
    for (int32 Y = 0; Y < 1025; ++Y)
    {
        LayerWeights[Y * 1025 + 1] = 255u;
    }
    STerrainComponent Terrain = MakeTerrain(1025, LayerWeights, 1);

    TVector<uint16> Heights(GTerrainPageTexelCount);
    TVector<uint16> Normals(GTerrainPageTexelCount);
    TVector<uint32> Weights(GTerrainPageTexelCount);
    // Depth 2 of a 1025 map holds mip 2 pages, four samples per texel, and node 5 is the page at the origin.
    TerrainPages::Build(*Terrain.GetStore(), MakeLayout(Terrain), 5u, Heights.GetData(), Normals.GetData(), Weights.GetData());

    const uint32 Origin = Weights[GTerrainPageGutter * GTerrainPageTexels + GTerrainPageGutter] & 0xFFu;
    EXPECT_GT(Origin, 0u);
    EXPECT_LT(Origin, 255u);
}
