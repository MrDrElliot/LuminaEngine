#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Net/NetPrediction.h"
#include "Net/NetWorldState.h"
#include "FakeCharacterScene.h"
#include "Components/NetworkComponent.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"

using namespace Lumina;

namespace
{
    constexpr uint32 Sender       = 5;
    constexpr uint32 OtherSender  = 6;
    constexpr uint32 Guid         = NetGUID_DynamicStart + 1;
    constexpr float  FixedDt      = 1.0f / 60.0f;

    struct FFixture
    {
        ECS::FRegistry   Registry;
        FNetWorldState&  State;
        FFakeCharacterScene Scene;
        ECS::FEntity     Entity;

        FFixture()
            : State(Registry.Ctx().Emplace<FNetWorldState>())
        {
            Entity = Registry.Create();
            State.GuidTable.Register(SNetGUID{ Guid }, Entity);

            SNetworkComponent& Net = Registry.Emplace<SNetworkComponent>(Entity);
            Net.NetGUID.Value = Guid;
            Net.OwningConnectionId = Sender;

            Registry.Emplace<SCharacterControllerComponent>(Entity);
            NetPrediction::RefreshServerPrediction(Registry, Entity, Sender);

            Scene.States[Entity] = Physics::FCharacterNetState{};
        }

        FNetPredictionServer& Prediction()
        {
            return Registry.Get<ECS::Write<FNetPredictionServer>>(Entity);
        }
    };

    TVector<uint8> BuildPacket()
    {
        const FNetMoveCommand Commands[3] =
        {
            NetPrediction::MakeMoveCommand(1, FVector3(1.0f, 0.0f, 0.0f), 0.0f, 0),
            NetPrediction::MakeMoveCommand(2, FVector3(1.0f, 0.0f, 0.0f), 0.0f, 0),
            NetPrediction::MakeMoveCommand(3, FVector3(1.0f, 0.0f, 0.0f), 0.0f, 0),
        };
        TVector<uint8> Buffer;
        NetPrediction::WriteMoveCommands(Buffer, Guid, Commands, 3);
        return Buffer;
    }
}

TEST(NetPredictionBudget, ReceiveCommandsStopsOnceTheBudgetDropsBelowOne)
{
    FFixture Fixture;
    Fixture.Prediction().Budget = 1.5f;
    const TVector<uint8> Packet = BuildPacket();

    NetPrediction::ReceiveCommands(Fixture.Registry, Fixture.Scene, FixedDt, Sender, Packet.GetData(), (SIZE_T)Packet.Num());

    ASSERT_EQ(Fixture.Scene.SimulateCalls.Num(), 1);
    EXPECT_FALSE(Fixture.Scene.SimulateCalls[0].bReplay);
    const FNetPredictionServer& Prediction = Fixture.Prediction();
    EXPECT_EQ(Prediction.LastProcessed, 1u);
    EXPECT_EQ(Prediction.AckSeq, 1u);
    EXPECT_TRUE(Prediction.bAckPending);
    EXPECT_NEAR(Prediction.Budget, 0.5f, 1.0e-4f);

    NetPrediction::ReceiveCommands(Fixture.Registry, Fixture.Scene, FixedDt, Sender, Packet.GetData(), (SIZE_T)Packet.Num());
    EXPECT_EQ(Fixture.Scene.SimulateCalls.Num(), 1);
    EXPECT_EQ(Fixture.Prediction().LastProcessed, 1u);
}

TEST(NetPredictionBudget, TickBudgetsRefillsEnoughForTheResentPacketToCoverTheNextMove)
{
    FFixture Fixture;
    Fixture.Prediction().Budget = 1.5f;
    const TVector<uint8> Packet = BuildPacket();

    NetPrediction::ReceiveCommands(Fixture.Registry, Fixture.Scene, FixedDt, Sender, Packet.GetData(), (SIZE_T)Packet.Num());
    ASSERT_EQ(Fixture.Scene.SimulateCalls.Num(), 1);

    NetPrediction::TickBudgets(Fixture.Registry, FixedDt, FixedDt);
    EXPECT_NEAR(Fixture.Prediction().Budget, 1.5f, 1.0e-4f);

    NetPrediction::ReceiveCommands(Fixture.Registry, Fixture.Scene, FixedDt, Sender, Packet.GetData(), (SIZE_T)Packet.Num());
    EXPECT_EQ(Fixture.Scene.SimulateCalls.Num(), 2);
    EXPECT_EQ(Fixture.Prediction().LastProcessed, 2u);
}

TEST(NetPredictionBudget, TickBudgetsNeverExceedsTheCap)
{
    FFixture Fixture;
    const float Cap = 0.25f / FixedDt + 4.0f;

    NetPrediction::TickBudgets(Fixture.Registry, FixedDt, 1000.0f);

    EXPECT_NEAR(Fixture.Prediction().Budget, Cap, 1.0e-4f);
}

TEST(NetPredictionBudget, TheHostClosesABacklogDeeperThanTheCapWhileTheClientKeepsMoving)
{
    FFixture Fixture;
    constexpr uint32 LostTicks = NetPrediction::MaxMovesPerUpload * 2 + 6;
    constexpr uint32 CatchUpTicks = 60;

    uint32 NewestSeq = 0;
    TVector<FNetMoveCommand> Window;
    TVector<uint8> Packet;
    auto Tick = [&](bool bDelivered)
    {
        NetPrediction::TickBudgets(Fixture.Registry, FixedDt, FixedDt);
        ++NewestSeq;
        if (!bDelivered)
        {
            return;
        }

        // The same window SendCommands uploads, the newest MaxMovesPerUpload moves the host has not confirmed.
        const uint32 Oldest = Math::Max(Fixture.Prediction().LastProcessed + 1, NewestSeq + 1 - Math::Min(NewestSeq, NetPrediction::MaxMovesPerUpload));
        Window.Reset();
        for (uint32 Seq = Oldest; Seq <= NewestSeq; ++Seq)
        {
            Window.Add(NetPrediction::MakeMoveCommand(Seq, FVector3(1.0f, 0.0f, 0.0f), 0.0f, 0));
        }
        Packet.Reset();
        NetPrediction::WriteMoveCommands(Packet, Guid, Window.GetData(), (uint32)Window.Num());
        NetPrediction::ReceiveCommands(Fixture.Registry, Fixture.Scene, FixedDt, Sender, Packet.GetData(), (SIZE_T)Packet.Num());
    };

    for (uint32 Index = 0; Index < LostTicks; ++Index)
    {
        Tick(false);
    }
    Tick(true);
    ASSERT_LT(Fixture.Prediction().LastProcessed, NewestSeq);

    uint32 TicksToCatchUp = 0;
    while (Fixture.Prediction().LastProcessed < NewestSeq && TicksToCatchUp < CatchUpTicks)
    {
        Tick(true);
        ++TicksToCatchUp;
    }
    EXPECT_EQ(Fixture.Prediction().LastProcessed, NewestSeq);
    EXPECT_GT(Fixture.Prediction().SkippedMoves, 0u);

    for (uint32 Index = 0; Index < CatchUpTicks; ++Index)
    {
        Tick(true);
        EXPECT_EQ(Fixture.Prediction().LastProcessed, NewestSeq);
    }
}

TEST(NetPredictionBudget, CommandsFromAConnectionThatDoesNotOwnTheEntityAreIgnored)
{
    FFixture Fixture;
    const TVector<uint8> Packet = BuildPacket();

    NetPrediction::ReceiveCommands(Fixture.Registry, Fixture.Scene, FixedDt, OtherSender, Packet.GetData(), (SIZE_T)Packet.Num());

    EXPECT_EQ(Fixture.Scene.SimulateCalls.Num(), 0);
    EXPECT_EQ(Fixture.Prediction().LastProcessed, 0u);
}
