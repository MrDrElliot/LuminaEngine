#include <gtest/gtest.h>

#include "GameplayTags/GameplayMessageBus.h"
#include "World/ECS/CommandBus.h"
#include "World/ECS/Registry.h"

using namespace Lumina;

namespace
{
    FGameplayTag Tag(const char* Name)
    {
        FGameplayTag Result;
        Result.TagName = FName(Name);
        return Result;
    }

    struct FBusFixture
    {
        ECS::FRegistry       Registry;
        ECS::FCommandBus     Commands{ Registry };
        FGameplayMessageBus  Bus{ Registry, Commands };
    };
}

TEST(GameplayMessageBus, BroadcastReachesPartialAncestorsButNotExactOnes)
{
    FBusFixture F;
    int32 Partial = 0;
    int32 Exact   = 0;
    int32 Leaf    = 0;

    F.Bus.Subscribe<FGameplayTag>(Tag("BusTest.Damage"), [&](const FGameplayTag&) { ++Partial; });
    F.Bus.Subscribe<FGameplayTag>(Tag("BusTest.Damage"), [&](const FGameplayTag&) { ++Exact; }, EGameplayTagMatch::Exact);
    F.Bus.Subscribe<FGameplayTag>(Tag("BusTest.Damage.Fire"), [&](const FGameplayTag& Message)
    {
        EXPECT_EQ(Message.TagName, FName("Payload"));
        ++Leaf;
    });

    F.Bus.Broadcast(Tag("BusTest.Damage.Fire"), Tag("Payload"));

    EXPECT_EQ(Partial, 1);
    EXPECT_EQ(Exact, 0);
    EXPECT_EQ(Leaf, 1);
}

TEST(GameplayMessageBus, ListenersOnlyHearTheirPayloadType)
{
    FBusFixture F;
    int32 Heard = 0;

    F.Bus.Subscribe<FGameplayTagContainer>(Tag("BusTest.Typed"), [&](const FGameplayTagContainer&) { ++Heard; });
    F.Bus.Broadcast(Tag("BusTest.Typed"), Tag("Payload"));

    EXPECT_EQ(Heard, 0);
}

TEST(GameplayMessageBus, SendUpStopsAtTheFirstHandler)
{
    FBusFixture F;
    const ECS::FEntity Root = F.Registry.Create();
    const ECS::FEntity Mid  = F.Registry.Create();
    const ECS::FEntity Leaf = F.Registry.Create();
    ASSERT_TRUE(F.Registry.AttachChild(Mid, Root));
    ASSERT_TRUE(F.Registry.AttachChild(Leaf, Mid));

    TVector<ECS::FEntity> Reached;
    F.Bus.Subscribe<FGameplayTag>(Leaf, Tag("BusTest.Up"), [&](const FGameplayTag&) { Reached.push_back(Leaf); return false; });
    F.Bus.Subscribe<FGameplayTag>(Mid,  Tag("BusTest.Up"), [&](const FGameplayTag&) { Reached.push_back(Mid); return true; });
    F.Bus.Subscribe<FGameplayTag>(Root, Tag("BusTest.Up"), [&](const FGameplayTag&) { Reached.push_back(Root); return false; });

    F.Bus.SendUp(Leaf, Tag("BusTest.Up"), Tag("Payload"));

    ASSERT_EQ(Reached.size(), 2u);
    EXPECT_EQ(Reached[0], Leaf);
    EXPECT_EQ(Reached[1], Mid);
}

TEST(GameplayMessageBus, SendDownWalksTheSubtreeInPreorder)
{
    FBusFixture F;
    const ECS::FEntity Root = F.Registry.Create();
    const ECS::FEntity A    = F.Registry.Create();
    const ECS::FEntity A1   = F.Registry.Create();
    const ECS::FEntity B    = F.Registry.Create();
    ASSERT_TRUE(F.Registry.AttachChild(A, Root));
    ASSERT_TRUE(F.Registry.AttachChild(B, Root));
    ASSERT_TRUE(F.Registry.AttachChild(A1, A));

    TVector<ECS::FEntity> Reached;
    for (ECS::FEntity Entity : { Root, A, A1, B })
    {
        F.Bus.Subscribe<FGameplayTag>(Entity, Tag("BusTest.Down"), [&Reached, Entity](const FGameplayTag&) { Reached.push_back(Entity); });
    }

    F.Bus.SendDown(Root, Tag("BusTest.Down"), Tag("Payload"), false);

    ASSERT_EQ(Reached.size(), 3u);
    EXPECT_EQ(Reached[0], A);
    EXPECT_EQ(Reached[1], A1);
    EXPECT_EQ(Reached[2], B);
}

TEST(GameplayMessageBus, ExactEntityListenerIgnoresChildChannels)
{
    FBusFixture F;
    const ECS::FEntity Target = F.Registry.Create();
    int32 Partial = 0;
    int32 Exact   = 0;
    F.Bus.Subscribe<FGameplayTag>(Target, Tag("BusTest.To"), [&](const FGameplayTag&) { ++Partial; });
    F.Bus.Subscribe<FGameplayTag>(Target, Tag("BusTest.To"), [&](const FGameplayTag&) { ++Exact; }, EGameplayTagMatch::Exact);

    F.Bus.SendTo(Target, Tag("BusTest.To.Child"), Tag("Payload"));

    EXPECT_EQ(Partial, 1);
    EXPECT_EQ(Exact, 0);
}

TEST(GameplayMessageBus, AHandlerMayUnsubscribeItselfAndOthersMidDispatch)
{
    FBusFixture F;
    uint64 SecondId = 0;
    uint64 FirstId  = 0;
    int32  First    = 0;
    int32  Second   = 0;

    FirstId = F.Bus.Subscribe<FGameplayTag>(Tag("BusTest.Reentry"), [&](const FGameplayTag&)
    {
        ++First;
        F.Bus.Unsubscribe(FirstId);
        F.Bus.Unsubscribe(SecondId);
    });
    SecondId = F.Bus.Subscribe<FGameplayTag>(Tag("BusTest.Reentry"), [&](const FGameplayTag&) { ++Second; });

    F.Bus.Broadcast(Tag("BusTest.Reentry"), Tag("Payload"));
    F.Bus.Broadcast(Tag("BusTest.Reentry"), Tag("Payload"));

    EXPECT_EQ(First, 1);
    EXPECT_EQ(Second, 0);
    EXPECT_EQ(F.Bus.NumListeners(), 0u);
}

TEST(GameplayMessageBus, ADeferredSendCarriesItsOwnCopyToTheFlush)
{
    FBusFixture F;
    int32 Heard = 0;
    F.Bus.Subscribe<FGameplayTagContainer>(Tag("BusTest.Deferred"), [&](const FGameplayTagContainer& Message)
    {
        ASSERT_EQ(Message.Num(), 1);
        EXPECT_EQ(Message.Tags[0].TagName, FName("Kept"));
        ++Heard;
    });

    {
        const ECS::FCommandBus::FDeferScope Defer(true);
        FGameplayTagContainer Payload;
        Payload.AddTag(Tag("Kept"));
        F.Bus.Broadcast(Tag("BusTest.Deferred"), Payload);
        Payload.Tags[0].TagName = FName("Changed");
    }

    EXPECT_EQ(Heard, 0);
    F.Commands.Flush();
    EXPECT_EQ(Heard, 1);
}

TEST(GameplayMessageBus, PruneDropsTheListenersOfDestroyedEntities)
{
    FBusFixture F;
    const ECS::FEntity Owner = F.Registry.Create();
    F.Bus.Subscribe<FGameplayTag>(Owner, Tag("BusTest.Prune"), [](const FGameplayTag&) {});
    F.Registry.Destroy(Owner);

    F.Bus.PruneDeadOwners();

    EXPECT_EQ(F.Bus.NumListeners(), 0u);
}
