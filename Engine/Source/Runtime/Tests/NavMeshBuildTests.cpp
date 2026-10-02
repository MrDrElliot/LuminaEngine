#include "gtest/gtest.h"

#include <cfloat>

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "AI/Navigation/NavTileStreamer.h"
#include "AI/Navigation/NavTypes.h"
#include <atomic>
#include <vector>

#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Platform/Time/PlatformTime.h"

// Exercises the Recast/Detour pipeline through the public builder, so no world or ECS is involved.

using namespace Lumina;

namespace
{
    // An axis-aligned ground quad at Y = Height, wound so its normal points up.
    void AddGroundQuad(FNavBuildInput& In, float MinX, float MinZ, float MaxX, float MaxZ, float Height)
    {
        const uint32 Base = (uint32)In.Vertices.size();
        In.Vertices.push_back(FVector3(MinX, Height, MinZ));
        In.Vertices.push_back(FVector3(MinX, Height, MaxZ));
        In.Vertices.push_back(FVector3(MaxX, Height, MaxZ));
        In.Vertices.push_back(FVector3(MaxX, Height, MinZ));

        In.Indices.push_back(Base + 0); In.Indices.push_back(Base + 1); In.Indices.push_back(Base + 2);
        In.Indices.push_back(Base + 0); In.Indices.push_back(Base + 2); In.Indices.push_back(Base + 3);
    }

    void GrowBounds(FNavBuildInput& In, float Pad)
    {
        In.BoundsMin = FVector3( FLT_MAX);
        In.BoundsMax = FVector3(-FLT_MAX);
        for (const FVector3& V : In.Vertices)
        {
            In.BoundsMin = Math::Min(In.BoundsMin, V);
            In.BoundsMax = Math::Max(In.BoundsMax, V);
        }
        In.BoundsMin -= FVector3(Pad);
        In.BoundsMax += FVector3(Pad);
    }

    void ApplyTestSettings(FNavBuildInput& In)
    {
        In.Settings.CellSize = 0.2f;
        In.Settings.CellHeight = 0.15f;
        In.Settings.AgentRadius = 0.3f;
        In.Settings.AgentHeight = 1.8f;
        In.Settings.TileSizeVoxels = 64;
    }

    FNavBuildInput MakeGroundPlane(float HalfSize = 12.0f)
    {
        FNavBuildInput In;
        ApplyTestSettings(In);
        AddGroundQuad(In, -HalfSize, -HalfSize, HalfSize, HalfSize, 0.0f);
        GrowBounds(In, 3.0f);
        return In;
    }

    // Two platforms with a gap too wide to walk, so only a link can join them.
    FNavBuildInput MakeSeparatedIslands()
    {
        FNavBuildInput In;
        ApplyTestSettings(In);
        AddGroundQuad(In, -10.0f, -6.0f, -2.0f, 6.0f, 0.0f);
        AddGroundQuad(In,   2.0f, -6.0f, 10.0f, 6.0f, 0.0f);
        GrowBounds(In, 3.0f);
        return In;
    }

    FNavOffMeshLink MakeGapLink()
    {
        FNavOffMeshLink Link;
        Link.Start  = FVector3(-2.5f, 0.0f, 0.0f);
        Link.End    = FVector3( 2.5f, 0.0f, 0.0f);
        Link.Radius = 0.8f;
        Link.Area   = (uint8)ENavArea::Ground;
        Link.Flags  = (uint16)ENavPolyFlag::Jump;
        return Link;
    }

    // Axis-aligned volume spanning [Min, Max] in XZ, tagged with Area.
    FNavAreaVolume MakeBoxVolume(float MinX, float MinZ, float MaxX, float MaxZ, float MinY, float MaxY, uint8 Area)
    {
        FNavAreaVolume Volume;
        Volume.Hull.push_back(FVector3(MinX, 0.0f, MinZ));
        Volume.Hull.push_back(FVector3(MinX, 0.0f, MaxZ));
        Volume.Hull.push_back(FVector3(MaxX, 0.0f, MaxZ));
        Volume.Hull.push_back(FVector3(MaxX, 0.0f, MinZ));
        Volume.MinY = MinY;
        Volume.MaxY = MaxY;
        Volume.Area = Area;
        return Volume;
    }

    TUniquePtr<FNavMesh> BakeAndHydrate(FNavBuildInput In)
    {
        FNavBuildOutput Out;
        if (!NavMeshBuilder::BakeSync(std::move(In), Out))
        {
            return nullptr;
        }

        auto Mesh = MakeUnique<FNavMesh>();
        if (!Mesh->Initialize(Out.Origin, Out.TileWorldSize, Out.MaxTiles, Out.MaxPolysPerTile, std::move(Out.Tiles)))
        {
            return nullptr;
        }
        return Mesh;
    }

    int32 CountTrianglesWithArea(const FNavMesh& Mesh, uint8 Area)
    {
        int32 Count = 0;
        Mesh.ForEachTriangle([&Count, Area](const FVector3&, const FVector3&, const FVector3&, uint8 TriArea)
        {
            if (TriArea == Area)
            {
                ++Count;
            }
        });
        return Count;
    }
}

TEST(NavMeshBuild, GroundPlaneBakesAndPathsAcross)
{
    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(MakeGroundPlane());
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    const FNavDebugStats Stats = Mesh->GetDebugStats();
    EXPECT_GT(Stats.Triangles, 0);

    FNavPath Path;
    FNavQueryFilter Filter;
    ASSERT_TRUE(Mesh->FindPath(FVector3(-10.0f, 0.0f, -10.0f), FVector3(10.0f, 0.0f, 10.0f), Filter, Path));
    EXPECT_TRUE(Path.bValid);
    EXPECT_FALSE(Path.bPartial);
    EXPECT_FALSE(Path.bTruncated);
    EXPECT_GE(Path.Corners.size(), 2u);
}

// Every partition has to give a walkable, connected plane, since the cheaper ones are offered as drop-in replacements.
TEST(NavMeshBuild, EveryRegionPartitionBakesAWalkablePlane)
{
    for (ENavRegionPartition Partition : { ENavRegionPartition::ProjectDefault, ENavRegionPartition::Watershed, ENavRegionPartition::Monotone, ENavRegionPartition::Layers })
    {
        FNavBuildInput In = MakeGroundPlane();
        In.Settings.Partition = Partition;
        TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(std::move(In));
        ASSERT_NE(Mesh, nullptr) << "partition " << (int32)Partition;
        ASSERT_TRUE(Mesh->IsReady()) << "partition " << (int32)Partition;

        FNavPath Path;
        FNavQueryFilter Filter;
        ASSERT_TRUE(Mesh->FindPath(FVector3(-10.0f, 0.0f, -10.0f), FVector3(10.0f, 0.0f, 10.0f), Filter, Path)) << "partition " << (int32)Partition;
        EXPECT_FALSE(Path.bPartial) << "partition " << (int32)Partition;
    }
}

TEST(NavMeshBuild, NullAreaVolumeSplitsThePlane)
{
    FNavBuildInput In = MakeGroundPlane();
    // A carved strip all the way across, wider than the agent diameter, so no route survives.
    In.AreaVolumes.push_back(MakeBoxVolume(-20.0f, -1.5f, 20.0f, 1.5f, -2.0f, 2.0f, (uint8)ENavArea::Null));

    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(std::move(In));
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    // Each side is still walkable on its own.
    FNavPath SameSide;
    FNavQueryFilter Filter;
    ASSERT_TRUE(Mesh->FindPath(FVector3(-10.0f, 0.0f, -10.0f), FVector3(10.0f, 0.0f, -10.0f), Filter, SameSide));
    EXPECT_FALSE(SameSide.bPartial);

    FNavPath Crossing;
    const bool bFound = Mesh->FindPath(FVector3(0.0f, 0.0f, -10.0f), FVector3(0.0f, 0.0f, 10.0f), Filter, Crossing);
    EXPECT_TRUE(!bFound || Crossing.bPartial);
}

TEST(NavMeshBuild, AreaVolumeTagsPolysWithItsArea)
{
    FNavBuildInput In = MakeGroundPlane();
    In.AreaVolumes.push_back(MakeBoxVolume(-6.0f, -6.0f, 6.0f, 6.0f, -2.0f, 2.0f, (uint8)ENavArea::Water));

    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(std::move(In));
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    EXPECT_GT(CountTrianglesWithArea(*Mesh, (uint8)ENavArea::Water), 0);
    EXPECT_GT(CountTrianglesWithArea(*Mesh, (uint8)ENavArea::Ground), 0);
}

TEST(NavMeshBuild, SeparatedIslandsHaveNoRouteWithoutALink)
{
    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(MakeSeparatedIslands());
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    FNavPath Path;
    FNavQueryFilter Filter;
    const bool bFound = Mesh->FindPath(FVector3(-4.0f, 0.0f, 0.0f), FVector3(4.0f, 0.0f, 0.0f), Filter, Path);
    EXPECT_TRUE(!bFound || Path.bPartial);
}

TEST(NavMeshBuild, OffMeshLinkBridgesSeparatedIslands)
{
    FNavBuildInput In = MakeSeparatedIslands();
    In.Links.push_back(MakeGapLink());

    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(std::move(In));
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());
    EXPECT_GE(Mesh->GetDebugStats().OffMeshLinks, 1);

    FNavPath Path;
    FNavQueryFilter Filter;
    ASSERT_TRUE(Mesh->FindPath(FVector3(-4.0f, 0.0f, 0.0f), FVector3(4.0f, 0.0f, 0.0f), Filter, Path));
    EXPECT_TRUE(Path.bValid);
    EXPECT_FALSE(Path.bPartial);
}

TEST(NavMeshBuild, OffMeshLinkIsSkippedWhenItsFlagIsExcluded)
{
    FNavBuildInput In = MakeSeparatedIslands();
    In.Links.push_back(MakeGapLink());

    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(std::move(In));
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    FNavQueryFilter NoJump;
    NoJump.IncludeFlags = (uint16)(0xFFFFu & ~(uint32)ENavPolyFlag::Jump);

    FNavPath Path;
    const bool bFound = Mesh->FindPath(FVector3(-4.0f, 0.0f, 0.0f), FVector3(4.0f, 0.0f, 0.0f), NoJump, Path);
    EXPECT_TRUE(!bFound || Path.bPartial);
}

TEST(NavMeshBuild, RaycastSeparatesClearLinesFromWalls)
{
    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(MakeGroundPlane());
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    FNavQueryFilter Filter;

    FNavRaycastResult Clear;
    ASSERT_TRUE(Mesh->Raycast(FVector3(-8.0f, 0.0f, 0.0f), FVector3(8.0f, 0.0f, 0.0f), Filter, Clear));
    EXPECT_FALSE(Clear.bHit);
    EXPECT_FLOAT_EQ(Clear.T, 1.0f);

    // Walking off the edge of the plane has to stop at the boundary.
    FNavRaycastResult Blocked;
    ASSERT_TRUE(Mesh->Raycast(FVector3(0.0f, 0.0f, 0.0f), FVector3(40.0f, 0.0f, 0.0f), Filter, Blocked));
    EXPECT_TRUE(Blocked.bHit);
    EXPECT_LT(Blocked.T, 1.0f);
    EXPECT_LT(Blocked.Point.x, 40.0f);
}

TEST(NavMeshBuild, BakeHandleOutlivesTheCallerReference)
{
    // Dropping the requester handle mid-bake used to free it under the worker.
    TSharedPtr<FNavBakeHandle> Handle = NavMeshBuilder::Bake(MakeGroundPlane(24.0f));
    ASSERT_NE(Handle, nullptr);

    TSharedPtr<FNavBakeHandle> Observer = Handle;
    Handle.reset();

    Observer->bCancelRequested.store(true, std::memory_order_release);
    while (!Observer->bDone.load(std::memory_order_acquire))
    {
        PlatformTime::YieldThread();
    }
    EXPECT_GE(Observer->Progress(), 0.0f);
}

TEST(NavMeshBuild, LargeGridExceedsTheThirtyTwoBitRefBudget)
{
    // 16-voxel tiles keep each Recast pass cheap while still crossing the tile count that matters.
    FNavBuildInput In;
    ApplyTestSettings(In);
    In.Settings.TileSizeVoxels = 16;
    AddGroundQuad(In, -30.0f, -30.0f, 30.0f, 30.0f, 0.0f);
    GrowBounds(In, 1.0f);

    FNavBuildOutput Out;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(std::move(In), Out));

    // 32-bit poly refs cap maxTiles at 256 once maxPolys eats 14 bits, so this is the interesting range.
    ASSERT_GT(Out.MaxTiles, 256);

    auto Mesh = MakeUnique<FNavMesh>();
    ASSERT_TRUE(Mesh->Initialize(Out.Origin, Out.TileWorldSize, Out.MaxTiles, Out.MaxPolysPerTile, std::move(Out.Tiles)));
    ASSERT_TRUE(Mesh->IsReady());

    const FNavDebugStats Stats = Mesh->GetDebugStats();
    EXPECT_GT(Stats.LoadedTiles, 256);

    FNavPath Path;
    FNavQueryFilter Filter;
    ASSERT_TRUE(Mesh->FindPath(FVector3(-28.0f, 0.0f, -28.0f), FVector3(28.0f, 0.0f, 28.0f), Filter, Path));
    EXPECT_TRUE(Path.bValid);
}

TEST(NavMeshBuild, TileBlobKeepsItsLinkArrayEightByteAligned)
{
    // dtLink holds a dtPolyRef, so under 64-bit refs the links block has to start 8-byte aligned.
    // sizeof(dtMeshHeader) is 100, so a stock dtAlign4 walk misaligns it whenever vertCount is even.
    FNavBuildOutput Out;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(MakeGroundPlane(20.0f), Out));

    int32 Checked = 0;
    for (const FNavTileData& Tile : Out.Tiles)
    {
        if (Tile.Blob.empty())
        {
            continue;
        }
        EXPECT_TRUE(NavMeshTesting::TileLinksAreAligned(Tile.Blob)) << "tile " << Tile.X << ", " << Tile.Y;
        ++Checked;
    }
    EXPECT_GT(Checked, 0);
}

TEST(NavMeshBuild, StaleTileBlobIsRejectedRatherThanRead)
{
    FNavBuildOutput Out;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(MakeGroundPlane(), Out));

    // Truncating leaves magic and version intact, which is all dtNavMesh::addTile checks. A blob
    // baked against a different Detour layout gets through the same gap.
    int32 Corrupted = 0;
    for (FNavTileData& Tile : Out.Tiles)
    {
        if (!Tile.Blob.empty())
        {
            Tile.Blob.resize(Tile.Blob.size() - 4);
            ++Corrupted;
        }
    }
    ASSERT_GT(Corrupted, 0);

    auto Mesh = MakeUnique<FNavMesh>();
    EXPECT_FALSE(Mesh->Initialize(Out.Origin, Out.TileWorldSize, Out.MaxTiles, Out.MaxPolysPerTile, std::move(Out.Tiles)));
}

TEST(NavMeshBuild, QueriesSurviveConcurrentRetiling)
{
    // removeTile frees the tile while pooled queries may be walking it, so this used to be a
    // use after free whenever a hot rebake landed next to a worker running FindPath.
    FNavBuildOutput Out;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(MakeGroundPlane(20.0f), Out));

    TVector<FNavTileData> Baked = Out.Tiles;
    auto Mesh = MakeUnique<FNavMesh>();
    ASSERT_TRUE(Mesh->Initialize(Out.Origin, Out.TileWorldSize, Out.MaxTiles, Out.MaxPolysPerTile, std::move(Out.Tiles)));

    const FNavTileData* Victim = nullptr;
    for (const FNavTileData& Tile : Baked)
    {
        if (!Tile.Blob.empty())
        {
            Victim = &Tile;
            break;
        }
    }
    ASSERT_NE(Victim, nullptr);

    std::atomic<bool> bStop{ false };
    std::atomic<uint64> Queries{ 0 };

    std::vector<Lumina::FThread> Readers;
    Readers.reserve(6);
    for (int32 i = 0; i < 6; ++i)
    {
        Readers.emplace_back([&bStop, &Queries, Raw = Mesh.get()]
        {
            FNavQueryFilter Filter;
            while (!bStop.load(std::memory_order_acquire))
            {
                FNavPath Path;
                Raw->FindPath(FVector3(-18.0f, 0.0f, -18.0f), FVector3(18.0f, 0.0f, 18.0f), Filter, Path);
                FVector3 Projected;
                Raw->ProjectPoint(FVector3(0.0f, 0.0f, 0.0f), FVector3(2.0f, 16.0f, 2.0f), Filter, Projected);
                Queries.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const uint64 StartEpoch = Mesh->GetTopologyEpoch();
    for (int32 i = 0; i < 400; ++i)
    {
        TVector<uint8> Copy = Victim->Blob;
        EXPECT_TRUE(Mesh->RebuildTile(Victim->X, Victim->Y, std::move(Copy)));
    }

    bStop.store(true, std::memory_order_release);
    for (Lumina::FThread& Reader : Readers)
    {
        Reader.Join();
    }

    EXPECT_GT(Queries.load(std::memory_order_relaxed), 0u);
    EXPECT_GT(Mesh->GetTopologyEpoch(), StartEpoch);

    // Retiling in place must leave the mesh answering exactly as it did before.
    FNavPath Path;
    FNavQueryFilter Filter;
    ASSERT_TRUE(Mesh->FindPath(FVector3(-18.0f, 0.0f, -18.0f), FVector3(18.0f, 0.0f, 18.0f), Filter, Path));
    EXPECT_TRUE(Path.bValid);
    EXPECT_EQ(Path.Epoch, Mesh->GetTopologyEpoch());
}

TEST(NavMeshBuild, TilesPageOutAndBackIn)
{
    FNavBuildOutput Out;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(MakeGroundPlane(20.0f), Out));

    TVector<FNavTileData> Baked = Out.Tiles;
    auto Mesh = MakeUnique<FNavMesh>();
    ASSERT_TRUE(Mesh->Initialize(Out.Origin, Out.TileWorldSize, Out.MaxTiles, Out.MaxPolysPerTile, std::move(Out.Tiles)));

    int32 Resident = Mesh->GetResidentTileCount();
    ASSERT_GT(Resident, 1);

    int32 Removed = 0;
    for (const FNavTileData& Tile : Baked)
    {
        if (Tile.Blob.empty())
        {
            continue;
        }
        ASSERT_TRUE(Mesh->HasTile(Tile.X, Tile.Y));
        EXPECT_TRUE(Mesh->RemoveTile(Tile.X, Tile.Y));
        EXPECT_FALSE(Mesh->HasTile(Tile.X, Tile.Y));
        ++Removed;
    }
    ASSERT_GT(Removed, 0);
    EXPECT_EQ(Mesh->GetResidentTileCount(), 0);

    for (const FNavTileData& Tile : Baked)
    {
        if (!Tile.Blob.empty())
        {
            EXPECT_TRUE(Mesh->AddTile(Tile.X, Tile.Y, Tile.Blob));
        }
    }
    EXPECT_EQ(Mesh->GetResidentTileCount(), Resident);

    FNavPath Path;
    FNavQueryFilter Filter;
    ASSERT_TRUE(Mesh->FindPath(FVector3(-18.0f, 0.0f, -18.0f), FVector3(18.0f, 0.0f, 18.0f), Filter, Path));
    EXPECT_TRUE(Path.bValid);
}

namespace
{
    struct FStreamFixture
    {
        FNavBuildOutput        Out;
        TUniquePtr<FNavMesh>   Mesh;
        FNavTileStreamer       Streamer;
        int32                  NonEmpty = 0;
    };

    // A plane wide enough to span many tiles, with the mesh left empty for the streamer to fill.
    bool MakeStreamFixture(FStreamFixture& Fx, int32 Budget)
    {
        FNavBuildInput In;
        ApplyTestSettings(In);
        In.Settings.TileSizeVoxels = 16;
        AddGroundQuad(In, -40.0f, -40.0f, 40.0f, 40.0f, 0.0f);
        GrowBounds(In, 1.0f);

        if (!NavMeshBuilder::BakeSync(std::move(In), Fx.Out))
        {
            return false;
        }
        for (const FNavTileData& Tile : Fx.Out.Tiles)
        {
            if (!Tile.Blob.empty())
            {
                ++Fx.NonEmpty;
            }
        }

        Fx.Mesh = MakeUnique<FNavMesh>();
        if (!Fx.Mesh->Initialize(Fx.Out.Origin, Fx.Out.TileWorldSize, Budget, Fx.Out.MaxPolysPerTile))
        {
            return false;
        }
        Fx.Streamer.Reset(Fx.Out.Origin, Fx.Out.TileWorldSize);
        return true;
    }

    FNavTileStreamer::FSettings StreamSettings(int32 Budget, float Radius)
    {
        FNavTileStreamer::FSettings S;
        S.MaxResidentTiles = Budget;
        S.LoadRadius       = Radius;
        // Converge in one call so a test asserts the steady state, not the ramp.
        S.MaxOpsPerTick    = 100000;
        return S;
    }
}

TEST(NavMeshStreaming, ZeroRadiusKeepsEveryTileResident)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeStreamFixture(Fx, 8192));
    ASSERT_GT(Fx.NonEmpty, 16);

    const TVector<FVector3> NoFocus;
    const FNavTileStreamer::FStats Stats = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, NoFocus, StreamSettings(8192, 0.0f));

    EXPECT_EQ(Stats.Resident, Fx.NonEmpty);
    EXPECT_EQ(Stats.Starved, 0);
    EXPECT_EQ(Fx.Mesh->GetResidentTileCount(), Fx.NonEmpty);
}

TEST(NavMeshStreaming, TilesFollowTheFocusPoint)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeStreamFixture(Fx, 8192));

    const FNavTileStreamer::FSettings Settings = StreamSettings(8192, 10.0f);

    TVector<FVector3> Focus;
    Focus.push_back(FVector3(-35.0f, 0.0f, -35.0f));
    const FNavTileStreamer::FStats Near = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, Focus, Settings);

    EXPECT_GT(Near.Resident, 0);
    EXPECT_LT(Near.Resident, Fx.NonEmpty);
    EXPECT_TRUE(Fx.Streamer.IsResident(0, 0));

    // A corner tile of the far side has no business being loaded from here.
    const int32 FarX = Fx.Out.TilesX - 1;
    const int32 FarY = Fx.Out.TilesY - 1;
    EXPECT_FALSE(Fx.Streamer.IsResident(FarX, FarY));

    FVector3 Projected;
    FNavQueryFilter Filter;
    EXPECT_TRUE(Fx.Mesh->ProjectPoint(FVector3(-35.0f, 0.0f, -35.0f), FVector3(2.0f, 16.0f, 2.0f), Filter, Projected));
    EXPECT_FALSE(Fx.Mesh->ProjectPoint(FVector3(35.0f, 0.0f, 35.0f), FVector3(2.0f, 16.0f, 2.0f), Filter, Projected));

    // Walking to the far corner has to swap the resident set over, not merely grow it.
    Focus[0] = FVector3(35.0f, 0.0f, 35.0f);
    const FNavTileStreamer::FStats Far = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, Focus, Settings);

    EXPECT_GT(Far.Removed, 0);
    EXPECT_TRUE(Fx.Streamer.IsResident(FarX, FarY));
    EXPECT_FALSE(Fx.Streamer.IsResident(0, 0));
    EXPECT_TRUE(Fx.Mesh->ProjectPoint(FVector3(35.0f, 0.0f, 35.0f), FVector3(2.0f, 16.0f, 2.0f), Filter, Projected));
}

TEST(NavMeshStreaming, BudgetClampsTheResidentSet)
{
    constexpr int32 Budget = 12;
    FStreamFixture Fx;
    ASSERT_TRUE(MakeStreamFixture(Fx, Budget));
    ASSERT_GT(Fx.NonEmpty, Budget);

    TVector<FVector3> Focus;
    Focus.push_back(FVector3(0.0f, 0.0f, 0.0f));
    const FNavTileStreamer::FStats Stats = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, Focus, StreamSettings(Budget, 1000.0f));

    EXPECT_LE(Stats.Resident, Budget);
    EXPECT_GT(Stats.Starved, 0);
    EXPECT_LE(Fx.Mesh->GetResidentTileCount(), Budget);
}

TEST(NavMeshStreaming, PerTickOpCapBoundsTheMutationWindow)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeStreamFixture(Fx, 8192));

    FNavTileStreamer::FSettings Settings = StreamSettings(8192, 0.0f);
    Settings.MaxOpsPerTick = 4;

    const TVector<FVector3> NoFocus;
    const FNavTileStreamer::FStats First = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, NoFocus, Settings);
    EXPECT_EQ(First.Added, 4);
    EXPECT_EQ(First.Resident, 4);

    // Repeated ticks converge on the full set rather than stalling.
    for (int32 i = 0; i < 1000 && Fx.Streamer.GetResidentCount() < Fx.NonEmpty; ++i)
    {
        Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, NoFocus, Settings);
    }
    EXPECT_EQ(Fx.Streamer.GetResidentCount(), Fx.NonEmpty);
}

TEST(NavMeshStreaming, HysteresisStopsBoundaryThrash)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeStreamFixture(Fx, 8192));

    FNavTileStreamer::FSettings Settings = StreamSettings(8192, 12.0f);
    Settings.KeepRadiusScale = 2.0f;

    TVector<FVector3> Focus;
    Focus.push_back(FVector3(0.0f, 0.0f, 0.0f));
    Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, Focus, Settings);
    const int32 Settled = Fx.Streamer.GetResidentCount();
    ASSERT_GT(Settled, 0);

    // Drifting just past the load radius must not evict anything, since keep radius is twice it.
    Focus[0] = FVector3(6.0f, 0.0f, 0.0f);
    const FNavTileStreamer::FStats Drift = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, Focus, Settings);
    EXPECT_EQ(Drift.Removed, 0);
    EXPECT_GE(Drift.Resident, Settled);
}

TEST(NavMeshBuild, BatchRebakeReproducesTheFullBake)
{
    // A hot rebake has to land byte for byte on what the full bake produced, or swapping a tile in
    // shifts geometry at the seams against its neighbours.
    FNavBuildInput In = MakeGroundPlane(24.0f);
    FNavBuildInput Copy = In;

    FNavBuildOutput Full;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(std::move(In), Full));
    ASSERT_GT(Full.Tiles.size(), 2u);

    TVector<FNavTileCoord> Coords;
    for (const FNavTileData& Tile : Full.Tiles)
    {
        Coords.push_back(FNavTileCoord{ Tile.X, Tile.Y });
    }

    FNavBuildOutput Layout = Full;
    Layout.Tiles.clear();

    TVector<FNavTileData> Batch;
    NavMeshBuilder::BakeTiles(Copy, Layout, Coords, Batch);
    ASSERT_EQ(Batch.size(), Full.Tiles.size());

    for (size_t i = 0; i < Batch.size(); ++i)
    {
        EXPECT_EQ(Batch[i].X, Full.Tiles[i].X);
        EXPECT_EQ(Batch[i].Y, Full.Tiles[i].Y);
        EXPECT_EQ(Batch[i].Blob, Full.Tiles[i].Blob) << "tile " << Batch[i].X << ", " << Batch[i].Y;
    }
}

TEST(NavMeshBuild, BatchRebakeIgnoresTilesItWasNotAskedFor)
{
    FNavBuildInput In = MakeGroundPlane(24.0f);
    FNavBuildInput Copy = In;

    FNavBuildOutput Full;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(std::move(In), Full));

    // One tile in the middle of the grid, so it has neighbours on every side to be confused with.
    const FNavTileData& Target = Full.Tiles[Full.Tiles.size() / 2];

    FNavBuildOutput Layout = Full;
    Layout.Tiles.clear();

    TVector<FNavTileCoord> Coords;
    Coords.push_back(FNavTileCoord{ Target.X, Target.Y });

    TVector<FNavTileData> Batch;
    NavMeshBuilder::BakeTiles(Copy, Layout, Coords, Batch);

    ASSERT_EQ(Batch.size(), 1u);
    EXPECT_EQ(Batch[0].X, Target.X);
    EXPECT_EQ(Batch[0].Y, Target.Y);
    EXPECT_EQ(Batch[0].Blob, Target.Blob);
}

namespace
{
    // As MakeStreamFixture, but hydrated holding every tile, which is what streaming switched off does.
    bool MakeSeededStreamFixture(FStreamFixture& Fx, int32 Budget)
    {
        FNavBuildInput In;
        ApplyTestSettings(In);
        In.Settings.TileSizeVoxels = 16;
        AddGroundQuad(In, -40.0f, -40.0f, 40.0f, 40.0f, 0.0f);
        GrowBounds(In, 1.0f);

        if (!NavMeshBuilder::BakeSync(std::move(In), Fx.Out))
        {
            return false;
        }
        for (const FNavTileData& Tile : Fx.Out.Tiles)
        {
            if (!Tile.Blob.empty())
            {
                ++Fx.NonEmpty;
            }
        }

        TVector<FNavTileData> Seed = Fx.Out.Tiles;
        Fx.Mesh = MakeUnique<FNavMesh>();
        if (!Fx.Mesh->Initialize(Fx.Out.Origin, Fx.Out.TileWorldSize, Budget, Fx.Out.MaxPolysPerTile, std::move(Seed)))
        {
            return false;
        }
        Fx.Streamer.Reset(Fx.Out.Origin, Fx.Out.TileWorldSize);
        return true;
    }
}

// Re-adding a seeded tile fails, and copies the whole blob under the exclusive lock to do so.
TEST(NavMeshStreaming, SeededTilesAreAdoptedRatherThanReAdded)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeSeededStreamFixture(Fx, 8192));
    ASSERT_GT(Fx.NonEmpty, 16);

    const TVector<FVector3> NoFocus;
    const FNavTileStreamer::FStats First = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, NoFocus, StreamSettings(8192, 0.0f));

    EXPECT_EQ(First.Added, 0) << "every tile was already in the mesh";
    EXPECT_EQ(First.Adopted, Fx.NonEmpty);
    EXPECT_EQ(First.Resident, Fx.NonEmpty);
    EXPECT_EQ(Fx.Mesh->GetResidentTileCount(), Fx.NonEmpty);

    // Converged, so a second pass must do no work at all.
    const FNavTileStreamer::FStats Second = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, NoFocus, StreamSettings(8192, 0.0f));
    EXPECT_EQ(Second.Added, 0);
    EXPECT_EQ(Second.Adopted, 0);
    EXPECT_EQ(Second.Removed, 0);
}

TEST(NavMeshStreaming, AdoptedTilesCanBeEvicted)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeSeededStreamFixture(Fx, 8192));

    const TVector<FVector3> NoFocus;
    Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, NoFocus, StreamSettings(8192, 0.0f));
    ASSERT_EQ(Fx.Streamer.GetResidentCount(), Fx.NonEmpty);

    TVector<FVector3> Focus;
    Focus.push_back(FVector3(-38.0f, 0.0f, -38.0f));
    const FNavTileStreamer::FStats Stats = Fx.Streamer.Update(*Fx.Mesh, Fx.Out.Tiles, Focus, StreamSettings(8192, 8.0f));

    EXPECT_GT(Stats.Removed, 0) << "a tile the streamer adopted must still be evictable";
    EXPECT_LT(Stats.Resident, Fx.NonEmpty);
    EXPECT_EQ(Fx.Mesh->GetResidentTileCount(), Stats.Resident);
}

// The epoch is mesh-wide, so without a spatial test one streamed tile repaths every agent in the world.
TEST(NavMeshBuild, TileChangeIsReportedOnlyForOverlappingBounds)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeSeededStreamFixture(Fx, 8192));

    const uint64 Before = Fx.Mesh->GetTopologyEpoch();

    const FNavTileData* Changed = nullptr;
    for (const FNavTileData& Tile : Fx.Out.Tiles)
    {
        if (!Tile.Blob.empty())
        {
            Changed = &Tile;
            break;
        }
    }
    ASSERT_NE(Changed, nullptr);
    ASSERT_TRUE(Fx.Mesh->RemoveTile(Changed->X, Changed->Y));
    EXPECT_GT(Fx.Mesh->GetTopologyEpoch(), Before);

    const float TileSize = Fx.Out.TileWorldSize;
    const FVector3 Min(Fx.Out.Origin.x + Changed->X * TileSize, 0.0f, Fx.Out.Origin.z + Changed->Y * TileSize);
    const FVector3 Max = Min + FVector3(TileSize, 0.0f, TileSize);
    EXPECT_TRUE(Fx.Mesh->HasTileChangedSince(Before, Min, Max));

    const FVector3 FarMin = Min + FVector3(TileSize * 8.0f, 0.0f, TileSize * 8.0f);
    const FVector3 FarMax = FarMin + FVector3(TileSize, 0.0f, TileSize);
    EXPECT_FALSE(Fx.Mesh->HasTileChangedSince(Before, FarMin, FarMax))
        << "a change under one tile must not invalidate a corridor eight tiles away";

    EXPECT_FALSE(Fx.Mesh->HasTileChangedSince(Fx.Mesh->GetTopologyEpoch(), Min, Max));
}

// More changes than the ring retains makes the answer unknowable, and unknowable has to read as changed.
TEST(NavMeshBuild, ChangeRingOverflowReportsChanged)
{
    FStreamFixture Fx;
    ASSERT_TRUE(MakeSeededStreamFixture(Fx, 8192));
    ASSERT_GT(Fx.NonEmpty, 4);

    const uint64 Before = Fx.Mesh->GetTopologyEpoch();

    const FNavTileData& Target = Fx.Out.Tiles[Fx.Out.Tiles.size() / 2];
    for (int32 i = 0; i < 600; ++i)
    {
        Fx.Mesh->RemoveTile(Target.X + 1000 + i, Target.Y + 1000);
    }

    const FVector3 FarMin(-1000.0f, 0.0f, -1000.0f);
    const FVector3 FarMax(-990.0f, 0.0f, -990.0f);
    EXPECT_TRUE(Fx.Mesh->HasTileChangedSince(Before, FarMin, FarMax));
}

TEST(NavMeshBuild, PathCornersCarryTheOffMeshLinkFlag)
{
    FNavBuildInput In = MakeSeparatedIslands();
    In.Links.push_back(MakeGapLink());

    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(std::move(In));
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    FNavPath Path;
    FNavQueryFilter Filter;
    ASSERT_TRUE(Mesh->FindPath(FVector3(-4.0f, 0.0f, 0.0f), FVector3(4.0f, 0.0f, 0.0f), Filter, Path));
    ASSERT_TRUE(Path.bValid);
    ASSERT_EQ(Path.CornerFlags.size(), Path.Corners.size());

    bool bSawLink = false;
    for (uint8 Flags : Path.CornerFlags)
    {
        bSawLink = bSawLink || (Flags & (uint8)ENavCornerFlag::OffMeshLink) != 0;
    }
    EXPECT_TRUE(bSawLink) << "the only route crosses the link, so a corner has to say so";
}

// A caller that keeps fewer corners than the query returns reads the overflow as arrival.
TEST(NavMeshBuild, CallerCornerCapTruncatesRatherThanLies)
{
    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(MakeGroundPlane(24.0f));
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    const FVector3 Start(-22.0f, 0.0f, -22.0f);
    const FVector3 End(22.0f, 0.0f, 22.0f);

    FNavQueryFilter Tight;
    Tight.MaxCorners = 8;
    FNavPath Capped;
    ASSERT_TRUE(Mesh->FindPath(Start, End, Tight, Capped));
    EXPECT_LE((int32)Capped.Corners.size(), 8);

    FNavQueryFilter Wide;
    FNavPath Full;
    ASSERT_TRUE(Mesh->FindPath(Start, End, Wide, Full));
    ASSERT_TRUE(Full.bValid);

    // An open plane straightens to very few corners, so the cap only has to hold where it does bite.
    if ((int32)Full.Corners.size() > 8)
    {
        EXPECT_TRUE(Capped.bTruncated);
        EXPECT_TRUE(Capped.bPartial);
    }
}

TEST(NavMeshBuild, PathResultNamesWhichEndWasOffTheNavMesh)
{
    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(MakeGroundPlane());
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    const FVector3 OnMesh(-10.0f, 0.0f, -10.0f);
    const FVector3 FarOff(1000.0f, 0.0f, 1000.0f);
    FNavQueryFilter Filter;

    FNavPath Good;
    ASSERT_TRUE(Mesh->FindPath(OnMesh, FVector3(10.0f, 0.0f, 10.0f), Filter, Good));
    EXPECT_EQ(Good.Result, ENavPathResult::Success);

    FNavPath BadEnd;
    EXPECT_FALSE(Mesh->FindPath(OnMesh, FarOff, Filter, BadEnd));
    EXPECT_EQ(BadEnd.Result, ENavPathResult::EndOffNavMesh);

    FNavPath BadStart;
    EXPECT_FALSE(Mesh->FindPath(FarOff, OnMesh, Filter, BadStart));
    EXPECT_EQ(BadStart.Result, ENavPathResult::StartOffNavMesh);
}

TEST(NavMeshBuild, PathResultReportsPartialForAnUnreachableGoal)
{
    TUniquePtr<FNavMesh> Mesh = BakeAndHydrate(MakeSeparatedIslands());
    ASSERT_NE(Mesh, nullptr);
    ASSERT_TRUE(Mesh->IsReady());

    FNavQueryFilter Filter;
    FNavPath Path;
    Mesh->FindPath(FVector3(-6.0f, 0.0f, 0.0f), FVector3(6.0f, 0.0f, 0.0f), Filter, Path);

    // Detour answers an unreachable goal with the closest poly it got to, not with nothing.
    EXPECT_EQ(Path.Result, ENavPathResult::Partial);
    EXPECT_TRUE(Path.bPartial);
}

TEST(NavMeshBuild, PathResultDefaultsToNotQueriedAndEveryReasonIsDistinct)
{
    const FNavPath Untouched;
    EXPECT_EQ(Untouched.Result, ENavPathResult::NotQueried);

    const ENavPathResult All[] = {
        ENavPathResult::NotQueried,     ENavPathResult::Success,
        ENavPathResult::NoNavMesh,      ENavPathResult::NavigationCompiledOut,
        ENavPathResult::QueryUnavailable, ENavPathResult::StartOffNavMesh,
        ENavPathResult::EndOffNavMesh,  ENavPathResult::NoRoute,
        ENavPathResult::CornersUnavailable, ENavPathResult::Partial,
        ENavPathResult::Truncated,
    };

    std::vector<std::string> Seen;
    for (ENavPathResult Result : All)
    {
        const char* Reason = ToString(Result);
        ASSERT_NE(Reason, nullptr);
        EXPECT_GT(strlen(Reason), 0u);
        EXPECT_EQ(std::find(Seen.begin(), Seen.end(), Reason), Seen.end()) << "duplicate reason: " << Reason;
        Seen.emplace_back(Reason);
    }
}

// A flat plane spanning thousands of tiles has to come back walkable everywhere. Partial coverage on a
// world-sized bake is the live symptom, so this is the smallest thing that reproduces it.
TEST(NavMeshBuild, WorldSizedPlaneIsWalkableInEveryTile)
{
    FNavBuildInput In = MakeGroundPlane(450.0f);

    FNavBuildOutput Out;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(std::move(In), Out));
    ASSERT_GT(Out.TilesX * Out.TilesY, 2000) << "grid too small to exercise the world-sized path";

    int32 Empty = 0;
    for (const FNavTileData& Tile : Out.Tiles)
    {
        if (Tile.Blob.empty())
        {
            ++Empty;
        }
    }

    // The outermost ring can legitimately be empty where the plane stops inside the padded bounds.
    const int32 Total    = (int32)Out.Tiles.size();
    const int32 EdgeRing = 2 * (Out.TilesX + Out.TilesY);
    EXPECT_LE(Empty, EdgeRing)
        << Empty << " of " << Total << " tiles baked empty over a flat plane (grid "
        << Out.TilesX << "x" << Out.TilesY << "); geometry is not reaching every tile";
}

// A large raised mesh sitting on ground must not leave a ring of empty tiles around its footprint. Whole
// tiles going empty is tile-granular, so it is a geometry-reaching-the-tile fault, not a slope one.
TEST(NavMeshBuild, LargeRaisedMeshLeavesNoRingOfEmptyTiles)
{
    FNavBuildInput In;
    ApplyTestSettings(In);

    // Ground everywhere, then a broad plateau over the middle with vertical sides.
    AddGroundQuad(In, -120.0f, -120.0f, 120.0f, 120.0f, 0.0f);
    AddGroundQuad(In,  -40.0f,  -40.0f,  40.0f,  40.0f, 6.0f);
    GrowBounds(In, 4.0f);

    FNavBuildOutput Out;
    ASSERT_TRUE(NavMeshBuilder::BakeSync(std::move(In), Out));
    ASSERT_GT(Out.TilesX * Out.TilesY, 100);

    // Mapped so a failure shows the shape of the hole rather than only its size.
    std::string Map;
    int32 Empty = 0;
    for (int32 Y = 0; Y < Out.TilesY; ++Y)
    {
        for (int32 X = 0; X < Out.TilesX; ++X)
        {
            bool bFound = false;
            for (const FNavTileData& Tile : Out.Tiles)
            {
                if (Tile.X == X && Tile.Y == Y)
                {
                    bFound = !Tile.Blob.empty();
                    break;
                }
            }
            Map += bFound ? '#' : '.';
            Empty += bFound ? 0 : 1;
        }
        Map += '\n';
    }

    const int32 EdgeRing = 2 * (Out.TilesX + Out.TilesY);
    EXPECT_LE(Empty, EdgeRing) << Empty << " empty tiles on a " << Out.TilesX << "x" << Out.TilesY
                               << " grid:\n" << Map;
}
