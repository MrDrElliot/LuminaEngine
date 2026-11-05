#include "gtest/gtest.h"

#include <cmath>

#include "AI/Navigation/NavOctree.h"
#include "Config/NavigationSettings.h"
#include "NavWorldTestUtil.h"
#include "World/Entity/Components/NavModifierComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"

using namespace Lumina;

namespace
{
    struct FBox
    {
        FVector3 Min;
        FVector3 Max;
    };

    bool Overlaps(const FBox& A, const FBox& B)
    {
        return A.Min.x <= B.Max.x && A.Max.x >= B.Min.x && A.Min.y <= B.Max.y && A.Max.y >= B.Min.y && A.Min.z <= B.Max.z && A.Max.z >= B.Min.z;
    }

    uint32 NextRandom(uint64& State)
    {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return (uint32)(State >> 16);
    }

    float RandomRange(uint64& State, float Min, float Max)
    {
        return Min + (Max - Min) * ((float)(NextRandom(State) & 0xFFFFFF) / (float)0xFFFFFF);
    }

    FBox RandomBox(uint64& State)
    {
        const FVector3 Center(RandomRange(State, -500.0f, 500.0f), RandomRange(State, -20.0f, 20.0f), RandomRange(State, -500.0f, 500.0f));
        // Mostly small with a few huge, the way rocks sit beside a terrain tile.
        const float Size = (NextRandom(State) % 50 == 0) ? RandomRange(State, 50.0f, 400.0f) : RandomRange(State, 0.2f, 6.0f);
        return FBox{ Center - FVector3(Size), Center + FVector3(Size) };
    }

    // The octree must answer exactly what a brute force scan would, through adds, moves and removals.
    void ExpectQueriesMatch(const FNavOctree& Octree, const TVector<FBox>& Boxes, const TVector<bool>& Alive, uint64& State)
    {
        for (int32 Query = 0; Query < 200; ++Query)
        {
            const FBox Region = RandomBox(State);
            TVector<uint32> Found;
            Octree.Query(Region.Min, Region.Max, [&Found](uint32 Payload, const FVector3&, const FVector3&) { Found.Add(Payload); });
            Algo::Sort(Found);

            TVector<uint32> Expected;
            for (int32 Index = 0; Index < Boxes.Num(); ++Index)
            {
                if (Alive[Index] && Overlaps(Boxes[Index], Region))
                {
                    Expected.Add(Index);
                }
            }
            ASSERT_EQ(Found, Expected);
        }
    }
}

TEST(NavOctree, QueriesMatchABruteForceScanThroughMovesAndRemovals)
{
    FNavOctree Octree;
    uint64 State = 12345;
    TVector<FBox> Boxes;
    TVector<uint32> Elements;
    TVector<bool> Alive;
    for (uint32 Index = 0; Index < 3000; ++Index)
    {
        Boxes.Add(RandomBox(State));
        Elements.Add(Octree.Add(Boxes.Last().Min, Boxes.Last().Max, Index));
        Alive.Add(true);
    }
    EXPECT_EQ(Octree.Num(), 3000);
    ExpectQueriesMatch(Octree, Boxes, Alive, State);

    for (int32 Index = 0; Index < Boxes.Num(); Index += 3)
    {
        Boxes[Index] = RandomBox(State);
        Octree.Update(Elements[Index], Boxes[Index].Min, Boxes[Index].Max);
    }
    for (int32 Index = 1; Index < Boxes.Num(); Index += 7)
    {
        Octree.Remove(Elements[Index]);
        Alive[Index] = false;
    }
    ExpectQueriesMatch(Octree, Boxes, Alive, State);
}

TEST(NavOctree, AnElementOutsideTheRootStillAnswersQueries)
{
    FNavOctree Octree(64.0f, 4.0f);
    Octree.Add(FVector3(1000.0f), FVector3(1001.0f), 7);
    int32 Hits = 0;
    Octree.Query(FVector3(999.0f), FVector3(1002.0f), [&Hits](uint32 Payload, const FVector3&, const FVector3&) { Hits += Payload == 7 ? 1 : 0; });
    EXPECT_EQ(Hits, 1);
}

// Recast voxelizes surfaces rather than volumes, so the obstacles stay lower than an agent or the ground inside them counts as walkable.
TEST(NavSourceTracking, AnObstacleMovedOntoOpenGroundRebakesBothItsTiles)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    const ECS::FEntity Crate = NavTest::SpawnBox(World.Registry(), FVector3(-15.0f, 0.7f, 0.0f), FVector3(2.0f, 0.7f, 2.0f));
    World.Start();
    ASSERT_TRUE(World.WaitReady());

    EXPECT_FALSE(World.IsGroundWalkable(-15.0f, 0.0f));
    EXPECT_TRUE(World.IsGroundWalkable(15.0f, 0.0f));

    World.Registry().Get<STransformComponent>(Crate).SetLocation(FVector3(15.0f, 0.7f, 0.0f));
    ASSERT_TRUE(World.WaitSettled());

    EXPECT_TRUE(World.IsGroundWalkable(-15.0f, 0.0f)) << "the ground the crate left opens again";
    EXPECT_FALSE(World.IsGroundWalkable(15.0f, 0.0f)) << "the ground it now stands on is carved";
    World.Stop();
}

TEST(NavSourceTracking, AColliderAddedAfterTheBakeCarvesTheMesh)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    World.Start();
    ASSERT_TRUE(World.WaitReady());
    EXPECT_TRUE(World.IsGroundWalkable(10.0f, 10.0f));

    NavTest::SpawnBox(World.Registry(), FVector3(10.0f, 0.7f, 10.0f), FVector3(2.0f, 0.7f, 2.0f));
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_FALSE(World.IsGroundWalkable(10.0f, 10.0f));
    World.Stop();
}

TEST(NavSourceTracking, ADestroyedColliderReopensTheGround)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    const ECS::FEntity Wall = NavTest::SpawnBox(World.Registry(), FVector3(0.0f, 0.7f, 12.0f), FVector3(3.0f, 0.7f, 3.0f));
    World.Start();
    ASSERT_TRUE(World.WaitReady());
    EXPECT_FALSE(World.IsGroundWalkable(0.0f, 12.0f));

    World.Registry().Destroy(Wall);
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_TRUE(World.IsGroundWalkable(0.0f, 12.0f));
    World.Stop();
}

// A streamed world can unload every source at once, and the sweep then rebuilds an empty order.
TEST(NavSourceTracking, TheSweepSurvivesEverySourceUnloading)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    World.Start();
    ASSERT_TRUE(World.WaitReady());
    for (int32 i = 0; i < 4; ++i)
    {
        World.Tick();
    }

    TVector<ECS::FEntity> Sources;
    for (ECS::FEntity Entity : World.Registry().View<SBoxColliderComponent>())
    {
        Sources.Add(Entity);
    }
    ASSERT_FALSE(Sources.IsEmpty());
    for (ECS::FEntity Entity : Sources)
    {
        World.Registry().Destroy(Entity);
    }

    for (int32 i = 0; i < 8; ++i)
    {
        World.Tick();
    }
    World.Stop();
}

TEST(NavSourceTracking, AColliderWrittenInPlaceIsFoundByTheSweep)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    const ECS::FEntity Pillar = NavTest::SpawnBox(World.Registry(), FVector3(-10.0f, 0.7f, -10.0f), FVector3(0.5f, 0.7f, 0.5f));
    World.Start();
    ASSERT_TRUE(World.WaitReady());
    EXPECT_TRUE(World.IsGroundWalkable(-12.5f, -10.0f));

    // A raw store raises no signal and moves nothing, which only the sweep can notice.
    World.Registry().Get<SBoxColliderComponent>(Pillar).HalfExtent = FVector3(4.0f, 0.7f, 4.0f);
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_FALSE(World.IsGroundWalkable(-12.5f, -10.0f));
    World.Stop();
}

TEST(NavSourceTracking, AnIdleWorldDirtiesNothing)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    for (int32 i = 0; i < 40; ++i)
    {
        NavTest::SpawnBox(World.Registry(), FVector3(-28.0f + (float)(i % 8) * 7.0f, 1.0f, -28.0f + (float)(i / 8) * 7.0f), FVector3(1.0f));
    }
    World.Start();
    ASSERT_TRUE(World.WaitReady());

    for (int32 i = 0; i < 30; ++i)
    {
        World.Tick();
    }
    EXPECT_TRUE(World.Nav().Runtime.DirtyTiles.IsEmpty());
    EXPECT_TRUE(World.Nav().Runtime.SettlingTiles.IsEmpty());
    EXPECT_TRUE(World.Nav().Runtime.PendingRebakes.IsEmpty());
    World.Stop();
}

namespace
{
    // Turns the sweep off for one test, so only moves and component signals can find a change.
    struct FNoSweep
    {
        int32 Saved = GetMutableDefault<CNavigationSettings>()->SourceVerifyBudget;
        FNoSweep() { GetMutableDefault<CNavigationSettings>()->SourceVerifyBudget = 0; }
        ~FNoSweep() { GetMutableDefault<CNavigationSettings>()->SourceVerifyBudget = Saved; }
    };
}

TEST(NavSourceTracking, MovesAndSignalsAloneFindEveryChange)
{
    FNoSweep NoSweep;
    NavTest::FNavWorld World;
    World.Create(64.0f);
    const ECS::FEntity Crate = NavTest::SpawnBox(World.Registry(), FVector3(-15.0f, 0.7f, 0.0f), FVector3(2.0f, 0.7f, 2.0f));
    World.Start();
    ASSERT_TRUE(World.WaitReady());

    World.Registry().Get<STransformComponent>(Crate).SetLocation(FVector3(15.0f, 0.7f, 0.0f));
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_TRUE(World.IsGroundWalkable(-15.0f, 0.0f)) << "the move channel carried the crate";
    EXPECT_FALSE(World.IsGroundWalkable(15.0f, 0.0f));

    const ECS::FEntity Added = NavTest::SpawnBox(World.Registry(), FVector3(0.0f, 0.7f, 15.0f), FVector3(2.0f, 0.7f, 2.0f));
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_FALSE(World.IsGroundWalkable(0.0f, 15.0f)) << "the construct signal carried the new collider";

    World.Registry().Get<SBoxColliderComponent>(Added).HalfExtent = FVector3(0.5f, 0.7f, 0.5f);
    World.Registry().Patch<SBoxColliderComponent>(Added);
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_TRUE(World.IsGroundWalkable(0.0f, 17.5f)) << "the update signal carried the patched size";

    World.Registry().Destroy(Added);
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_TRUE(World.IsGroundWalkable(0.0f, 15.0f)) << "the destroy signal reopened the ground";
    World.Stop();
}

namespace
{
    // Holds one navigation setting for a test and puts it back after.
    template <typename T>
    struct TSettingScope
    {
        T& Setting;
        T  Saved;
        TSettingScope(T& InSetting, T Value) : Setting(InSetting), Saved(InSetting) { Setting = Value; }
        ~TSettingScope() { Setting = Saved; }
    };

    ECS::FEntity SpawnModifier(ECS::FRegistry& Registry, const FVector3& Location, const FVector3& Extents)
    {
        const ECS::FEntity Entity = Registry.Create();
        FTransform Transform;
        Transform.SetLocation(Location);
        Registry.Emplace<STransformComponent>(Entity, Transform);
        SNavModifierComponent& Modifier = Registry.Emplace<SNavModifierComponent>(Entity);
        Modifier.Area = ENavArea::Null;
        Modifier.Extents = Extents;
        return Entity;
    }
}

TEST(NavSourceTracking, AModifierMovingBackOverATileRestampsItFromTheCache)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    const ECS::FEntity Modifier = SpawnModifier(World.Registry(), FVector3(-15.0f, 0.0f, 0.0f), FVector3(2.0f, 2.0f, 2.0f));
    World.Start();
    ASSERT_TRUE(World.WaitReady());
    EXPECT_FALSE(World.IsGroundWalkable(-15.0f, 0.0f));

    World.Registry().Get<STransformComponent>(Modifier).SetLocation(FVector3(15.0f, 0.0f, 0.0f));
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_TRUE(World.IsGroundWalkable(-15.0f, 0.0f));
    EXPECT_FALSE(World.IsGroundWalkable(15.0f, 0.0f));

    World.Registry().Get<STransformComponent>(Modifier).SetLocation(FVector3(-15.0f, 0.0f, 0.0f));
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_FALSE(World.IsGroundWalkable(-15.0f, 0.0f)) << "the cached heightfield still takes the stamp";
    EXPECT_TRUE(World.IsGroundWalkable(15.0f, 0.0f));
    ASSERT_TRUE(World.Nav().Runtime.TileCache);
    EXPECT_GT(World.Nav().Runtime.TileCache->GetHits(), 0) << "the second move only restamped tiles the first one cached";
    World.Stop();
}

TEST(NavSourceTracking, AColliderMovedOnTerrainCarvesTheSlopeItLandsOn)
{
    NavTest::FNavWorld World;
    World.Create(64.0f, 256.0f, true);
    auto Ground = [](float X, float Z) { return NavTest::GroundHeight(X, Z); };
    const ECS::FEntity Crate = NavTest::SpawnBox(World.Registry(), FVector3(-15.0f, Ground(-15.0f, 0.0f) + 0.7f, 0.0f), FVector3(2.0f, 0.7f, 2.0f));
    World.Start();
    ASSERT_TRUE(World.WaitReady());
    EXPECT_FALSE(World.IsGroundWalkable(-15.0f, 0.0f, Ground(-15.0f, 0.0f)));
    EXPECT_TRUE(World.IsGroundWalkable(15.0f, 0.0f, Ground(15.0f, 0.0f)));

    World.Registry().Get<STransformComponent>(Crate).SetLocation(FVector3(15.0f, Ground(15.0f, 0.0f) + 0.7f, 0.0f));
    ASSERT_TRUE(World.WaitSettled());
    EXPECT_TRUE(World.IsGroundWalkable(-15.0f, 0.0f, Ground(-15.0f, 0.0f))) << "the rebake tessellated the terrain it left";
    EXPECT_FALSE(World.IsGroundWalkable(15.0f, 0.0f, Ground(15.0f, 0.0f)));
    World.Stop();
}

TEST(PathFollow, ACrowdOwingPathsIsServedAFewPerTick)
{
    TSettingScope<int32> Budget(GetMutableDefault<CNavigationSettings>()->MaxRepathsPerTick, 4);
    NavTest::FNavWorld World;
    World.Create(64.0f);
    World.Start(true);
    ASSERT_TRUE(World.WaitReady());

    TVector<ECS::FEntity> Agents;
    for (int32 i = 0; i < 20; ++i)
    {
        const ECS::FEntity Agent = World.Registry().Create();
        FTransform Transform;
        Transform.SetLocation(FVector3(-25.0f + (float)i * 2.5f, 0.1f, -20.0f));
        World.Registry().Emplace<STransformComponent>(Agent, Transform);
        World.Registry().Emplace<SPathFollowComponent>(Agent).SetTargetLocation(FVector3(-25.0f + (float)i * 2.5f, 0.0f, 20.0f));
        Agents.Add(Agent);
    }
    auto WithPaths = [&]
    {
        return (int32)Algo::CountIf(Agents.begin(), Agents.end(), [&](ECS::FEntity Agent) { return World.Registry().Get<SPathFollowComponent>(Agent).CornerCount > 0; });
    };

    World.TickFollow();
    EXPECT_EQ(WithPaths(), 4);
    for (int32 Tick = 0; Tick < 4; ++Tick)
    {
        World.TickFollow();
    }
    EXPECT_EQ(WithPaths(), 20) << "every agent is served once the round comes back to it";
    World.Stop();
}

TEST(PathFollow, AFollowerWithoutATransformIsSkipped)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    World.Start(true);
    ASSERT_TRUE(World.WaitReady());

    ECS::FRegistry& R = World.Registry();
    const ECS::FEntity Bare = R.Create();
    R.Emplace<SPathFollowComponent>(Bare).SetTargetLocation(FVector3(0.0f, 0.0f, 12.0f));

    const ECS::FEntity Placed = R.Create();
    R.Emplace<STransformComponent>(Placed).SetLocation(FVector3(0.0f, 0.0f, -12.0f));
    R.Emplace<SPathFollowComponent>(Placed).SetTargetLocation(FVector3(0.0f, 0.0f, 12.0f));

    for (int32 Tick = 0; Tick < 3; ++Tick)
    {
        World.TickFollow();
    }

    EXPECT_EQ(R.Get<SPathFollowComponent>(Bare).CornerCount, 0);
    EXPECT_EQ(R.Get<SPathFollowComponent>(Bare).Status, EPathFollowStatus::Searching);
    EXPECT_TRUE(R.Get<SPathFollowComponent>(Placed).IsFollowing());
    World.Stop();
}

namespace
{
    // An agent that never moves on its own, so only replanning changes its corners.
    ECS::FEntity SpawnFollower(NavTest::FNavWorld& World, const FVector3& At, const FVector3& Goal)
    {
        ECS::FRegistry& R = World.Registry();
        const ECS::FEntity Agent = R.Create();
        R.Emplace<STransformComponent>(Agent).SetLocation(At);
        SPathFollowComponent& Follow = R.Emplace<SPathFollowComponent>(Agent);
        Follow.bDriveCharacterController = false;
        Follow.SetTargetLocation(Goal);
        return Agent;
    }

    // Whether any leg of the agent's remaining route crosses the box, tested along each leg at small steps.
    bool RouteCrosses(const NavTest::FNavWorld& World, ECS::FEntity Agent, const FVector3& BoxMin, const FVector3& BoxMax)
    {
        const SPathFollowComponent& Follow = World.Registry().Get<SPathFollowComponent>(Agent);
        FVector3 From = World.Registry().Get<STransformComponent>(Agent).GetWorldLocationCached();
        for (int32 i = Follow.CurrentCorner; i < Follow.CornerCount; ++i)
        {
            const FVector3 To = Follow.PathCorners[i];
            for (int32 Step = 0; Step <= 64; ++Step)
            {
                const FVector3 P = Math::Mix(From, To, (float)Step / 64.0f);
                if (P.x > BoxMin.x && P.x < BoxMax.x && P.z > BoxMin.z && P.z < BoxMax.z)
                {
                    return true;
                }
            }
            From = To;
        }
        return false;
    }
}

// A wall raised across a path after it was found has to send the follower around it, not through it.
TEST(NavPathRepath, AWallBuiltAcrossAPathIsWalkedAround)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    World.Start(true);
    ASSERT_TRUE(World.WaitReady());

    const ECS::FEntity Agent = SpawnFollower(World, FVector3(0.0f, 0.0f, -12.0f), FVector3(0.0f, 0.0f, 12.0f));
    World.TickFollow();
    ASSERT_TRUE(World.Registry().Get<SPathFollowComponent>(Agent).IsFollowing());

    const FVector3 Half(8.0f, 0.7f, 1.0f);
    NavTest::SpawnBox(World.Registry(), FVector3(0.0f, 0.7f, 0.0f), Half);
    ASSERT_TRUE(World.WaitSettled());
    for (int32 Tick = 0; Tick < 4; ++Tick)
    {
        World.TickFollow();
    }

    EXPECT_FALSE(RouteCrosses(World, Agent, -Half, Half)) << "the follower still walks its old path through the new wall";
    World.Stop();
}

// The builder stands against what it built, so its own spot is carved away and the replan has to start from the nearest ground.
TEST(NavPathRepath, AnAgentStandingAgainstANewWallStillReplans)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    World.Start(true);
    ASSERT_TRUE(World.WaitReady());

    const ECS::FEntity Agent = SpawnFollower(World, FVector3(0.0f, 0.0f, -1.15f), FVector3(0.0f, 0.0f, 12.0f));
    World.TickFollow();
    ASSERT_TRUE(World.Registry().Get<SPathFollowComponent>(Agent).IsFollowing());

    const FVector3 Half(8.0f, 0.7f, 1.0f);
    NavTest::SpawnBox(World.Registry(), FVector3(0.0f, 0.7f, 0.0f), Half);
    ASSERT_TRUE(World.WaitSettled());
    for (int32 Tick = 0; Tick < 4; ++Tick)
    {
        World.TickFollow();
    }

    const SPathFollowComponent& Follow = World.Registry().Get<SPathFollowComponent>(Agent);
    EXPECT_FALSE(RouteCrosses(World, Agent, -Half, Half)) << "status " << (int32)Follow.Status << ", failures " << Follow.ConsecutiveFailures;
    World.Stop();
}

// The builder can stand on the site it is building, so finishing it carves away the very ground the agent is on.
TEST(NavPathRepath, AnAgentInsideANewObstacleStillReplans)
{
    NavTest::FNavWorld World;
    World.Create(64.0f);
    World.Start(true);
    ASSERT_TRUE(World.WaitReady());

    const ECS::FEntity Agent = SpawnFollower(World, FVector3(0.0f, 0.0f, -0.5f), FVector3(0.0f, 0.0f, 12.0f));
    World.TickFollow();
    ASSERT_TRUE(World.Registry().Get<SPathFollowComponent>(Agent).IsFollowing());

    const FVector3 Half(8.0f, 0.7f, 1.0f);
    NavTest::SpawnBox(World.Registry(), FVector3(0.0f, 0.7f, 0.0f), Half);
    ASSERT_TRUE(World.WaitSettled());
    for (int32 Tick = 0; Tick < 4; ++Tick)
    {
        World.TickFollow();
    }

    const SPathFollowComponent& Follow = World.Registry().Get<SPathFollowComponent>(Agent);
    EXPECT_EQ(Follow.Status, EPathFollowStatus::Following) << "failures " << Follow.ConsecutiveFailures;
    bool bRoundsTheEnd = false;
    for (int32 i = 0; i < Follow.CornerCount; ++i)
    {
        bRoundsTheEnd |= std::fabs(Follow.PathCorners[i].x) > Half.x;
    }
    EXPECT_TRUE(bRoundsTheEnd) << "the replan has to leave the wall and go around one of its ends";
    World.Stop();
}

// The navmesh may not promise a ledge the character cannot step, or a low building gets a walkable top the agent walks into.
TEST(NavPathRepath, ALedgeAboveTheClimbHeightIsNotWalkedOnto)
{
    for (const auto& [Height, bReachable] : { std::pair{ 0.3f, true }, std::pair{ 0.5f, false } })
    {
        NavTest::FNavWorld World;
        World.Create(64.0f);
        NavTest::SpawnBox(World.Registry(), FVector3(0.0f, Height * 0.5f, 0.0f), FVector3(4.0f, Height * 0.5f, 4.0f));
        World.Start(true);
        ASSERT_TRUE(World.WaitReady());

        const ECS::FEntity Agent = SpawnFollower(World, FVector3(0.0f, 0.0f, -10.0f), FVector3(0.0f, Height, 0.0f));
        World.TickFollow();
        const SPathFollowComponent& Follow = World.Registry().Get<SPathFollowComponent>(Agent);
        EXPECT_EQ(!Follow.IsPathPartial(), bReachable) << "slab " << Height << " m against a 0.4 m climb";
        World.Stop();
    }
}
