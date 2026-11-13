#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Core/Object/ObjectCore.h"
#include "Net/NetPrediction.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "FakeCharacterScene.h"
#include "Components/NetworkComponent.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"

using namespace Lumina;

namespace
{
    constexpr uint32 OwnerA = 2;
    constexpr uint32 OwnerB = 3;
    constexpr uint32 LongSessionSeq = 36000;

    struct FRecordingTransport final : INetworkTransport
    {
        TVector<TVector<uint8>> Sent;

        ENetworkBackend GetBackend() const override { return ENetworkBackend::ENet; }
        bool StartServer(const FListenParams&) override { return false; }
        FConnectionHandle ConnectToServer(const FConnectParams&) override { return {}; }
        bool IsServer() const override { return false; }
        void Disconnect(FConnectionHandle, uint32, bool) override {}
        void Broadcast(const void*, SIZE_T, uint8, ESendMode) override {}
        void Service(TVector<FNetworkEvent>&) override {}
        EConnectionState GetConnectionState(FConnectionHandle) const override { return EConnectionState::Connected; }

        bool Send(FConnectionHandle, const void* Data, SIZE_T Size, uint8, ESendMode) override
        {
            const uint8* Bytes = static_cast<const uint8*>(Data);
            Sent.Add(TVector<uint8>(Bytes, Bytes + Size));
            return true;
        }
    };

    FNetPredictionServer& ServeLongSession(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        NetPrediction::RefreshServerPrediction(Registry, Entity, OwnerA);
        FNetPredictionServer& Prediction = Registry.Get<ECS::Write<FNetPredictionServer>>(Entity);
        Prediction.LastProcessed = LongSessionSeq;
        Prediction.AckSeq = LongSessionSeq;
        Prediction.bAckPending = true;
        return Prediction;
    }
}

TEST(NetPrediction, ANewOwnerRestartsTheHostsMoveSequence)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    ServeLongSession(Registry, Entity);

    NetPrediction::RefreshServerPrediction(Registry, Entity, OwnerB);

    const FNetPredictionServer& Prediction = Registry.Get<ECS::Read<FNetPredictionServer>>(Entity);
    const uint32 FirstSeqFromB = FNetPredictionClient{}.NextSeq;
    EXPECT_EQ(Prediction.OwningConnectionId, OwnerB);
    EXPECT_EQ(Prediction.LastProcessed, 0u);
    EXPECT_GT(FirstSeqFromB, Prediction.LastProcessed);
    EXPECT_FALSE(Prediction.bAckPending);
    EXPECT_EQ(Prediction.AckSeq, 0u);
}

TEST(NetPrediction, AnUnchangedOwnerKeepsTheHostsMoveSequence)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    ServeLongSession(Registry, Entity);

    NetPrediction::RefreshServerPrediction(Registry, Entity, OwnerA);

    const FNetPredictionServer& Prediction = Registry.Get<ECS::Read<FNetPredictionServer>>(Entity);
    EXPECT_EQ(Prediction.LastProcessed, LongSessionSeq);
    EXPECT_EQ(Prediction.AckSeq, LongSessionSeq);
    EXPECT_TRUE(Prediction.bAckPending);
}

TEST(NetPrediction, LosingTheOwnerRemovesTheHostsMoveSequence)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    ServeLongSession(Registry, Entity);

    NetPrediction::RefreshServerPrediction(Registry, Entity, 0);

    EXPECT_FALSE(Registry.HasAll<FNetPredictionServer>(Entity));
}

TEST(NetPrediction, ACharacterStepRecordsOnePendingMoveWithItsResult)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<FNetPredictionClient>(Entity);
    Registry.Emplace<SCharacterControllerComponent>(Entity);
    SCharacterMovementComponent& Movement = Registry.Emplace<SCharacterMovementComponent>(Entity);
    Movement.bHasPendingMoveInput = true;
    Movement.PendingMoveDirection = FVector3(1.0f, 0.0f, 0.0f);
    Movement.PendingMoveThrottle  = 1.0f;
    Movement.MoveSpeed            = 6.0f;

    FFakeCharacterScene Scene;
    NetPrediction::OnCharacterStep(Registry, Scene, false);
    Scene.SimulateCharacterStep(Entity, Physics::FCharacterMoveInput{ .Direction = Movement.PendingMoveDirection,
        .Throttle = Movement.PendingMoveThrottle, .MoveSpeed = Movement.MoveSpeed }, 1.0f / 60.0f, false);
    NetPrediction::OnCharacterStep(Registry, Scene, true);

    const FNetPredictionClient& Prediction = Registry.Get<ECS::Read<FNetPredictionClient>>(Entity);
    ASSERT_EQ(Prediction.Pending.Num(), 1);
    const FPredictedMove& Move = Prediction.Pending[0];
    const Physics::FCharacterNetState& Stepped = Scene.States[Entity];
    EXPECT_EQ(Move.Command.Seq, 1u);
    EXPECT_EQ(Prediction.NextSeq, 2u);
    EXPECT_TRUE(Move.bHasAfter);
    EXPECT_GT(Stepped.Position.x, 0.0f);
    EXPECT_EQ(Move.After.Position, Stepped.Position);
    EXPECT_EQ(Move.After.Velocity, Stepped.Velocity);
}

TEST(NetPrediction, ABacklogDeeperThanOneUploadResyncsTheHostAndRebaselinesTheClient)
{
    RegisterPendingTypes();
    constexpr uint32 Guid = NetGUID_DynamicStart + 1;
    constexpr float FixedDt = 1.0f / 60.0f;

    ECS::FRegistry Registry;
    FNetWorldState& State = Registry.Ctx().Emplace<FNetWorldState>();
    TUniquePtr<FRecordingTransport> Recorder = MakeUnique<FRecordingTransport>();
    FRecordingTransport* Transport = Recorder.Get();
    State.Transport = Move(Recorder);

    const ECS::FEntity Entity = Registry.Create();
    State.GuidTable.Register(SNetGUID{ Guid }, Entity);
    SNetworkComponent& Net = Registry.Emplace<SNetworkComponent>(Entity);
    Net.NetGUID.Value = Guid;
    Net.OwningConnectionId = OwnerA;
    Registry.Emplace<SCharacterControllerComponent>(Entity);
    Registry.Emplace<FNetPredictionClient>(Entity);
    NetPrediction::RefreshServerPrediction(Registry, Entity, OwnerA);
    SCharacterMovementComponent& Movement = Registry.Emplace<SCharacterMovementComponent>(Entity);
    Movement.bHasPendingMoveInput = true;
    Movement.PendingMoveDirection = FVector3(1.0f, 0.0f, 0.0f);
    Movement.PendingMoveThrottle  = 1.0f;
    Movement.MoveSpeed            = FFakeCharacterScene::DefaultMoveSpeed;

    FFakeCharacterScene ClientScene;
    FFakeCharacterScene HostScene;
    auto ClientStep = [&]
    {
        NetPrediction::OnCharacterStep(Registry, ClientScene, false);
        ClientScene.SimulateCharacterStep(Entity, Physics::FCharacterMoveInput{ .Direction = Movement.PendingMoveDirection,
            .Throttle = Movement.PendingMoveThrottle, .MoveSpeed = Movement.MoveSpeed }, FixedDt, false);
        NetPrediction::OnCharacterStep(Registry, ClientScene, true);
    };
    auto Deliver = [&]
    {
        for (const TVector<uint8>& Packet : Transport->Sent)
        {
            Net::ForEachFramedMessage(Packet.GetData(), (SIZE_T)Packet.Num(), [&](const uint8* Msg, SIZE_T Size)
            {
                if (Msg[0] == static_cast<uint8>(ENetMessage::MoveCommands))
                {
                    NetPrediction::ReceiveCommands(Registry, HostScene, FixedDt, OwnerA, Msg, Size);
                }
                else if (Msg[0] == static_cast<uint8>(ENetMessage::MoveAck))
                {
                    NetPrediction::ReceiveAck(Registry, State, ClientScene, FixedDt, Msg, Size);
                }
            });
        }
        Transport->Sent.Reset();
    };
    auto NetworkFrame = [&]
    {
        NetPrediction::SendCommands(Registry, State);
        NetPrediction::TickBudgets(Registry, FixedDt, FixedDt);
        Deliver();
        NetPrediction::SendAcks(Registry, State, FixedDt);
        Deliver();
    };

    constexpr uint32 StalledMoves = NetPrediction::MaxMovesPerUpload * 2 + 6;
    for (uint32 Step = 0; Step < StalledMoves; ++Step)
    {
        ClientStep();
    }
    for (uint32 Frame = 0; Frame < 30; ++Frame)
    {
        ClientStep();
        NetworkFrame();
    }
    for (uint32 Frame = 0; Frame < 60; ++Frame)
    {
        NetworkFrame();
    }

    const FNetPredictionClient& Client = Registry.Get<ECS::Read<FNetPredictionClient>>(Entity);
    const FNetPredictionServer& Host = Registry.Get<ECS::Read<FNetPredictionServer>>(Entity);
    EXPECT_GE(Host.SkippedMoves, StalledMoves - NetPrediction::MaxMovesPerUpload);
    EXPECT_EQ(Host.LastProcessed, Client.NextSeq - 1);
    EXPECT_TRUE(Client.Pending.IsEmpty());
    EXPECT_GT(Client.Corrections, 0u);
    EXPECT_NEAR(ClientScene.States[Entity].Position.x, HostScene.States[Entity].Position.x, 0.02f);
    EXPECT_NEAR(ClientScene.States[Entity].Velocity.x, HostScene.States[Entity].Velocity.x, 0.1f);
}
