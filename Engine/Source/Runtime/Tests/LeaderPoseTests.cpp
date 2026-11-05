#include <gtest/gtest.h>
#include "Animation/SkeletalMeshUtils.h"
#include "World/ECS/Registry.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/FollowerPoseComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    ECS::FEntity MakeMesh(ECS::FRegistry& Registry, ECS::FEntity Parent = ECS::NullEntity)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<STransformComponent>(Entity);
        Registry.Emplace<SSkeletalMeshComponent>(Entity);
        if (Parent != ECS::NullEntity)
        {
            ECS::Utils::ReparentEntity(Registry, Entity, Parent);
        }
        return Entity;
    }

    void Follow(ECS::FRegistry& Registry, ECS::FEntity Follower, ECS::FEntity Leader = ECS::NullEntity)
    {
        Registry.Emplace<SFollowerPoseComponent>(Follower).Leader = Leader;
    }
}

TEST(LeaderPose, AnExplicitLeaderIsFollowed)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Body = MakeMesh(Registry);
    const ECS::FEntity Helmet = MakeMesh(Registry);
    Follow(Registry, Helmet, Body);

    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, Helmet), Body);
}

TEST(LeaderPose, AnEmptyLeaderFollowsTheNearestParentWithASkeletalMesh)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Body = MakeMesh(Registry);

    const ECS::FEntity Socket = Registry.Create();
    Registry.Emplace<STransformComponent>(Socket);
    ECS::Utils::ReparentEntity(Registry, Socket, Body);

    const ECS::FEntity Armor = MakeMesh(Registry, Socket);
    Follow(Registry, Armor);

    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, Armor), Body);
}

TEST(LeaderPose, AFollowerOfAFollowerReadsTheRootOfTheChain)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Body = MakeMesh(Registry);
    const ECS::FEntity Armor = MakeMesh(Registry, Body);
    const ECS::FEntity Cape = MakeMesh(Registry, Armor);
    Follow(Registry, Armor);
    Follow(Registry, Cape);

    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, Cape), Body);
}

TEST(LeaderPose, NothingToFollowResolvesToNull)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Loose = MakeMesh(Registry);
    Follow(Registry, Loose);

    const ECS::FEntity Unlinked = MakeMesh(Registry);

    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, Loose), ECS::FEntity{ECS::NullEntity});
    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, Unlinked), ECS::FEntity{ECS::NullEntity});
}

TEST(LeaderPose, ACycleResolvesToNull)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity A = MakeMesh(Registry);
    const ECS::FEntity B = MakeMesh(Registry);
    Follow(Registry, A, B);
    Follow(Registry, B, A);

    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, A), ECS::FEntity{ECS::NullEntity});
}

TEST(LeaderPose, ADestroyedLeaderResolvesToNull)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Body = MakeMesh(Registry);
    const ECS::FEntity Helmet = MakeMesh(Registry);
    Follow(Registry, Helmet, Body);
    Registry.Destroy(Body);

    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, Helmet), ECS::FEntity{ECS::NullEntity});
}

TEST(LeaderPose, AChainBrokenPastTheFirstHopResolvesToNull)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Body = MakeMesh(Registry);
    const ECS::FEntity Armor = MakeMesh(Registry);
    const ECS::FEntity Helmet = MakeMesh(Registry);
    Follow(Registry, Armor, Body);
    Follow(Registry, Helmet, Armor);
    Registry.Destroy(Body);

    EXPECT_EQ(SkeletalUtils::ResolvePoseLeader(Registry, Helmet), ECS::FEntity{ECS::NullEntity})
        << "the armor is still a follower, so copying its pose would race the pass that writes it";
}
