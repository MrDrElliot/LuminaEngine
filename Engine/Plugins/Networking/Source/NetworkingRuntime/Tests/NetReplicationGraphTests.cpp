#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"

#include "Components/NetworkComponent.h"
#include "Net/NetReplicationGraph.h"

using namespace Lumina;

namespace
{
    FNetExtract MakeExtract(const TVector<FVector3>& WorldPos)
    {
        FNetExtract Extract;
        for (int32 i = 0; i < WorldPos.Num(); ++i)
        {
            Extract.Guid.Add(static_cast<uint32>(i + 1));
            Extract.Pos.Add(WorldPos[i]);
            Extract.WorldPos.Add(WorldPos[i]);
            Extract.Rot.Add(NetQuantize::FQuantizedQuat::FromQuat(FQuat::Identity()));
            Extract.Scale.Add(NetQuantize::FQuantizedVector::FromVector(FVector3(1.0f), NetQuantize::ScaleQuantum));
            Extract.OwnerConn.Add(0);
            Extract.Flags.Add(NETREC_None);
            Extract.Pitch.Add(0);
            Extract.Seq.Emplace();
        }
        return Extract;
    }
}

TEST(NetReplicationGraph, BuildGridPlacesEachRecordInItsOwnCellRange)
{
    SDefaultWorldSettings Settings;
    Settings.WorldHalfExtent = 128.0f;
    Settings.GridCellSize    = 64.0f;

    // Four corners of the grid, each falling in a distinct cell.
    FNetExtract Extract = MakeExtract({
        FVector3(-120.0f, 0.0f, -120.0f),
        FVector3( 120.0f, 0.0f, -120.0f),
        FVector3(-120.0f, 0.0f,  120.0f),
        FVector3( 120.0f, 0.0f,  120.0f),
    });

    FNetGrid Grid;
    NetGraph::BuildGrid(Extract, Settings, Grid);

    ASSERT_EQ(Grid.DimX, 4);
    ASSERT_EQ(Grid.DimZ, 4);

    for (uint32 Rec = 0; Rec < Extract.Num(); ++Rec)
    {
        int32 cx, cz;
        Grid.CellCoords(Extract.WorldPos[Rec], cx, cz);
        const int32 Cell = Grid.CellIndex(cx, cz);

        bool bFound = false;
        for (int32 s = Grid.CellStart[Cell]; s < Grid.CellStart[Cell + 1]; ++s)
        {
            if (Grid.SortedRecords[s] == Rec)
            {
                bFound = true;
                EXPECT_EQ(Grid.SortedWorldPos[s], Extract.WorldPos[Rec]);
                EXPECT_EQ(Grid.SortedGuid[s], Extract.Guid[Rec]);
                break;
            }
        }
        EXPECT_TRUE(bFound) << "record " << Rec << " missing from its own cell range";
    }
}

TEST(NetReplicationGraph, BuildGridWidensCellsPastMaxDim)
{
    SDefaultWorldSettings Settings;
    const float Half = 8192.0f;
    Settings.WorldHalfExtent = Half;
    Settings.GridCellSize    = 1.0f; // (2*Half)/1 = 16384 cells per axis, past FNetGrid::MaxDim

    FNetExtract Extract = MakeExtract({ FVector3(0.0f) });

    FNetGrid Grid;
    NetGraph::BuildGrid(Extract, Settings, Grid);

    EXPECT_EQ(Grid.DimX, FNetGrid::MaxDim);
    EXPECT_EQ(Grid.DimZ, FNetGrid::MaxDim);
    EXPECT_GE(Grid.CellSize * static_cast<float>(FNetGrid::MaxDim), 2.0f * Half);
}

namespace
{
    SDefaultWorldSettings MakeDueSettings()
    {
        SDefaultWorldSettings Settings;
        Settings.TransformKeyframeInterval = 0.5f;
        Settings.TierMidRate = 10.0f;
        Settings.TierFarRate = 3.0f;
        return Settings;
    }
}

TEST(NetReplicationGraph, IsTransformDueForcedBaselineAlwaysSends)
{
    const SDefaultWorldSettings Settings = MakeDueSettings();
    FRelevantEntry Entry;
    Entry.TimeSinceSent = 0.0f;
    FNetChangeSeq Current = Entry.SentSeq; // unchanged

    bool bOutScale = false;
    EXPECT_TRUE(NetGraph::IsTransformDue(Entry, Current, /*bForceBaseline*/ true, Settings, bOutScale));
    EXPECT_TRUE(bOutScale);
}

TEST(NetReplicationGraph, IsTransformDueNeedsBaselineSendsUnchanged)
{
    const SDefaultWorldSettings Settings = MakeDueSettings();
    FRelevantEntry Entry;
    Entry.bNeedsBaseline = true;
    Entry.TimeSinceSent  = 0.0f;
    FNetChangeSeq Current = Entry.SentSeq;

    bool bOutScale = false;
    EXPECT_TRUE(NetGraph::IsTransformDue(Entry, Current, false, Settings, bOutScale));
    EXPECT_TRUE(bOutScale);
}

TEST(NetReplicationGraph, IsTransformDueKeyframeRefreshResendsUnchangedPose)
{
    const SDefaultWorldSettings Settings = MakeDueSettings();
    FRelevantEntry Entry;
    Entry.TimeSinceSent = Settings.TransformKeyframeInterval;
    FNetChangeSeq Current = Entry.SentSeq;

    bool bOutScale = false;
    EXPECT_TRUE(NetGraph::IsTransformDue(Entry, Current, false, Settings, bOutScale));

    Entry.TimeSinceSent = Settings.TransformKeyframeInterval * 0.5f;
    EXPECT_FALSE(NetGraph::IsTransformDue(Entry, Current, false, Settings, bOutScale))
        << "below the keyframe interval an unchanged pose stays quiet";
}

TEST(NetReplicationGraph, IsTransformDueThrottlesMidAndFarTiersToCadence)
{
    SDefaultWorldSettings Settings = MakeDueSettings();
    Settings.TransformKeyframeInterval = 0.0f; // isolate the cadence gate from the refresh gate

    FRelevantEntry MidEntry;
    MidEntry.Tier = ENetLODTier::Mid;
    FNetChangeSeq Current;
    ++Current.Pose; // changed since last send

    const float MidPeriod = 1.0f / Settings.TierMidRate;
    MidEntry.TimeSinceSent = MidPeriod * 0.5f;
    bool bOutScale = false;
    EXPECT_FALSE(NetGraph::IsTransformDue(MidEntry, Current, false, Settings, bOutScale))
        << "a changed Mid entry still waits for its cadence";

    MidEntry.TimeSinceSent = MidPeriod;
    EXPECT_TRUE(NetGraph::IsTransformDue(MidEntry, Current, false, Settings, bOutScale));

    FRelevantEntry FarEntry;
    FarEntry.Tier = ENetLODTier::Far;
    const float FarPeriod = 1.0f / Settings.TierFarRate;
    FarEntry.TimeSinceSent = FarPeriod * 0.5f;
    EXPECT_FALSE(NetGraph::IsTransformDue(FarEntry, Current, false, Settings, bOutScale))
        << "a changed Far entry still waits for its cadence";

    FarEntry.TimeSinceSent = FarPeriod;
    EXPECT_TRUE(NetGraph::IsTransformDue(FarEntry, Current, false, Settings, bOutScale));
}

namespace
{
    ECS::FEntity MakeOwnedEntity(ECS::FRegistry& Registry, uint32 OwnerConn, uint32 NetGuid)
    {
        ECS::FEntity Entity = Registry.Create();
        SNetworkComponent& Net = Registry.Emplace<SNetworkComponent>(Entity);
        Net.NetGUID            = SNetGUID{ NetGuid };
        Net.OwningConnectionId = OwnerConn;
        Registry.Emplace<STransformComponent>(Entity);
        return Entity;
    }
}

TEST(NetReplicationGraph, MultiOwnedConnectionAnchorIsStableAcrossUnrelatedChurn)
{
    ECS::FRegistry Registry;
    constexpr uint32 OwnerConn = 7;
    const ECS::FEntity Churn = MakeOwnedEntity(Registry, 0, NetGUID_DynamicStart + 3);
    MakeOwnedEntity(Registry, OwnerConn, NetGUID_DynamicStart + 1);
    MakeOwnedEntity(Registry, OwnerConn, NetGUID_DynamicStart + 2);

    FNetExtract Extract;
    THashMap<uint32, uint32> OwnerToRecord;
    NetGraph::BuildExtract(Registry, Extract, OwnerToRecord);

    const uint32* Rec = OwnerToRecord.Find(OwnerConn);
    ASSERT_NE(Rec, nullptr);
    const uint32 AnchorGuid = Extract.Guid[*Rec];

    // Swap-removing the front slot moves the last owned entity ahead of the other one.
    Registry.Destroy(Churn);

    NetGraph::BuildExtract(Registry, Extract, OwnerToRecord);
    Rec = OwnerToRecord.Find(OwnerConn);
    ASSERT_NE(Rec, nullptr);
    EXPECT_EQ(Extract.Guid[*Rec], AnchorGuid) << "the anchor must not flip with no change to either owned entity";
}

TEST(NetReplicationGraph, SingleOwnedEntityIsAlwaysItsConnectionsAnchor)
{
    ECS::FRegistry Registry;
    constexpr uint32 OwnerConn = 3;
    MakeOwnedEntity(Registry, OwnerConn, NetGUID_DynamicStart + 1);

    FNetExtract Extract;
    THashMap<uint32, uint32> OwnerToRecord;
    NetGraph::BuildExtract(Registry, Extract, OwnerToRecord);

    const uint32* Rec = OwnerToRecord.Find(OwnerConn);
    ASSERT_NE(Rec, nullptr);
    EXPECT_EQ(Extract.Guid[*Rec], NetGUID_DynamicStart + 1);
}

TEST(NetReplicationGraph, TierForDistanceSqBoundaries)
{
    SDefaultWorldSettings Settings;
    Settings.TierNearDistance = 30.0f;
    Settings.TierMidDistance  = 80.0f;
    Settings.AOILeaveRadius   = 150.0f;

    EXPECT_EQ(NetGraph::TierForDistanceSq(Settings.TierNearDistance * Settings.TierNearDistance, Settings), ENetLODTier::Near);
    EXPECT_EQ(NetGraph::TierForDistanceSq(Settings.TierNearDistance * Settings.TierNearDistance + 1.0f, Settings), ENetLODTier::Mid);
    EXPECT_EQ(NetGraph::TierForDistanceSq(Settings.TierMidDistance * Settings.TierMidDistance, Settings), ENetLODTier::Mid);
    EXPECT_EQ(NetGraph::TierForDistanceSq(Settings.TierMidDistance * Settings.TierMidDistance + 1.0f, Settings), ENetLODTier::Far);
    EXPECT_EQ(NetGraph::TierForDistanceSq(Settings.AOILeaveRadius * Settings.AOILeaveRadius, Settings), ENetLODTier::Far);
    EXPECT_EQ(NetGraph::TierForDistanceSq(Settings.AOILeaveRadius * Settings.AOILeaveRadius + 1.0f, Settings), ENetLODTier::Cull);
}
