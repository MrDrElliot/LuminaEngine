#include <gtest/gtest.h>

#include "HierarchicalTags/GameplayMessageBus.h"
#include "World/ECS/CommandBus.h"
#include "World/ECS/Registry.h"

using namespace Lumina;

namespace
{
    SHierarchicalTag Tag(const char* Name)
    {
        SHierarchicalTag Result;
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

    F.Bus.Subscribe<SHierarchicalTag>(Tag("BusTest.Damage"), [&](const SHierarchicalTag&) { ++Partial; });
    F.Bus.Subscribe<SHierarchicalTag>(Tag("BusTest.Damage"), [&](const SHierarchicalTag&) { ++Exact; }, EHierarchicalTagMatch::Exact);
    F.Bus.Subscribe<SHierarchicalTag>(Tag("BusTest.Damage.Fire"), [&](const SHierarchicalTag& Message)
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

    F.Bus.Subscribe<SHierarchicalTagContainer>(Tag("BusTest.Typed"), [&](const SHierarchicalTagContainer&) { ++Heard; });
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
    F.Bus.Subscribe<SHierarchicalTag>(Leaf, Tag("BusTest.Up"), [&](const SHierarchicalTag&) { Reached.Add(Leaf); return false; });
    F.Bus.Subscribe<SHierarchicalTag>(Mid,  Tag("BusTest.Up"), [&](const SHierarchicalTag&) { Reached.Add(Mid); return true; });
    F.Bus.Subscribe<SHierarchicalTag>(Root, Tag("BusTest.Up"), [&](const SHierarchicalTag&) { Reached.Add(Root); return false; });

    F.Bus.SendUp(Leaf, Tag("BusTest.Up"), Tag("Payload"));

    ASSERT_EQ(Reached.Num(), 2);
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
        F.Bus.Subscribe<SHierarchicalTag>(Entity, Tag("BusTest.Down"), [&Reached, Entity](const SHierarchicalTag&) { Reached.Add(Entity); });
    }

    F.Bus.SendDown(Root, Tag("BusTest.Down"), Tag("Payload"), false);

    ASSERT_EQ(Reached.Num(), 3);
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
    F.Bus.Subscribe<SHierarchicalTag>(Target, Tag("BusTest.To"), [&](const SHierarchicalTag&) { ++Partial; });
    F.Bus.Subscribe<SHierarchicalTag>(Target, Tag("BusTest.To"), [&](const SHierarchicalTag&) { ++Exact; }, EHierarchicalTagMatch::Exact);

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

    FirstId = F.Bus.Subscribe<SHierarchicalTag>(Tag("BusTest.Reentry"), [&](const SHierarchicalTag&)
    {
        ++First;
        F.Bus.Unsubscribe(FirstId);
        F.Bus.Unsubscribe(SecondId);
    });
    SecondId = F.Bus.Subscribe<SHierarchicalTag>(Tag("BusTest.Reentry"), [&](const SHierarchicalTag&) { ++Second; });

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
    F.Bus.Subscribe<SHierarchicalTagContainer>(Tag("BusTest.Deferred"), [&](const SHierarchicalTagContainer& Message)
    {
        ASSERT_EQ(Message.Num(), 1);
        EXPECT_EQ(Message.Tags[0].TagName, FName("Kept"));
        ++Heard;
    });

    {
        const ECS::FCommandBus::FDeferScope Defer(true);
        SHierarchicalTagContainer Payload;
        Payload.AddTag(Tag("Kept"));
        F.Bus.Broadcast(Tag("BusTest.Deferred"), Payload);
        Payload.Tags[0].TagName = FName("Changed");
    }

    EXPECT_EQ(Heard, 0);
    F.Commands.Flush();
    EXPECT_EQ(Heard, 1);
}

TEST(GameplayMessageBus, DestroyingTheOwnerDropsItsListenersImmediately)
{
    FBusFixture F;
    const ECS::FEntity Owner = F.Registry.Create();
    F.Bus.Subscribe<SHierarchicalTag>(Owner, Tag("BusTest.Prune"), [](const SHierarchicalTag&) {});

    F.Registry.Destroy(Owner);

    EXPECT_EQ(F.Bus.NumListeners(), 0u);
}
