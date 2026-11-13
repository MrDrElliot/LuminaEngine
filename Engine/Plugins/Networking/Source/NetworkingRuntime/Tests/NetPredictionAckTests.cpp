#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Net/NetPrediction.h"
#include "Net/NetWorldState.h"
#include "FakeCharacterScene.h"

using namespace Lumina;

namespace
{
    constexpr uint32 Guid = 7;
    constexpr uint32 MoveCount = 5;
    constexpr uint32 AckedSeq = 3;
    constexpr float FixedDt = 1.0f / 60.0f;
    constexpr float Epsilon = 1.0e-4f;

    struct FAckFixture
    {
        ECS::FRegistry          Registry;
        FNetWorldState          State;
        FFakeCharacterScene     Scene;
        ECS::FEntity            Entity = Registry.Create();

        // Each move walks along +X at Seq meters per second, so replayed calls name their move by MoveSpeed.
        FAckFixture()
        {
            State.GuidTable.Register(SNetGUID{ Guid }, Entity);
            FNetPredictionClient& Prediction = Registry.Emplace<FNetPredictionClient>(Entity);
            Scene.States[Entity] = Physics::FCharacterNetState{};
            for (uint32 Seq = 1; Seq <= MoveCount; ++Seq)
            {
                FPredictedMove Move{ NetPrediction::MakeMoveCommand(Seq, FVector3(1.0f, 0.0f, 0.0f), 0.0f, 0), (float)Seq, {}, false };
                Scene.SimulateCharacterStep(Entity, Physics::FCharacterMoveInput{ .Direction = FVector3(1.0f, 0.0f, 0.0f),
                    .Throttle = 1.0f, .MoveSpeed = Move.MoveSpeed }, FixedDt, false);
                Move.bHasAfter = Scene.GetCharacterNetState(Entity, Move.After);
                Prediction.Pending.Add(Move);
            }
            Prediction.NextSeq = MoveCount + 1;
            Scene.SimulateCalls.Reset();
        }

        FNetPredictionClient& Prediction()
        {
            return Registry.Get<ECS::Write<FNetPredictionClient>>(Entity);
        }

        void Ack(uint32 Seq, const Physics::FCharacterNetState& Host)
        {
            TVector<uint8> Buffer;
            NetPrediction::WriteMoveAck(Buffer, Guid, Seq, Host);
            NetPrediction::ReceiveAck(Registry, State, Scene, FixedDt, Buffer.GetData(), Buffer.Num());
        }
    };

    Physics::FCharacterNetState DisagreeingHost()
    {
        Physics::FCharacterNetState Host;
        Host.Position = FVector3(0.6f, 0.0f, 0.25f);
        Host.Velocity = FVector3(3.0f, 0.0f, 0.0f);
        return Host;
    }
}

TEST(NetPredictionAck, ADisagreeingAckRewindsToTheHostAndReplaysLaterMoves)
{
    FAckFixture Fixture;
    const Physics::FCharacterNetState Host = DisagreeingHost();

    Fixture.Ack(AckedSeq, Host);

    EXPECT_EQ(Fixture.Scene.SetStateCalls, 1u);
    ASSERT_EQ(Fixture.Scene.SimulateCalls.Num(), 2);
    for (uint32 Call = 0; Call < 2; ++Call)
    {
        EXPECT_TRUE(Fixture.Scene.SimulateCalls[Call].bReplay);
        EXPECT_EQ(Fixture.Scene.SimulateCalls[Call].Input.MoveSpeed, (float)(AckedSeq + 1 + Call));
    }

    const FNetPredictionClient& Prediction = Fixture.Prediction();
    ASSERT_EQ(Prediction.Pending.Num(), 2);
    FVector3 Expected = Host.Position;
    for (uint32 Index = 0; Index < 2; ++Index)
    {
        const FPredictedMove& Move = Prediction.Pending[Index];
        const uint32 Seq = AckedSeq + 1 + Index;
        Expected.x += (float)Seq * FixedDt;
        EXPECT_EQ(Move.Command.Seq, Seq);
        EXPECT_TRUE(Move.bHasAfter);
        EXPECT_NEAR(Move.After.Position.x, Expected.x, Epsilon);
        EXPECT_NEAR(Move.After.Position.z, Expected.z, Epsilon);
        EXPECT_NEAR(Move.After.Velocity.x, (float)Seq, Epsilon);
    }
    EXPECT_EQ(Prediction.Pending.Last().After.Position, Fixture.Scene.States[Fixture.Entity].Position);
    EXPECT_EQ(Prediction.LastAcked, AckedSeq);
    EXPECT_EQ(Prediction.Corrections, 1u);
}

TEST(NetPredictionAck, AnAgreeingAckOnlyTrimsConfirmedMoves)
{
    FAckFixture Fixture;
    const Physics::FCharacterNetState Last = Fixture.Scene.States[Fixture.Entity];

    Fixture.Ack(AckedSeq, Fixture.Prediction().Pending[AckedSeq - 1].After);

    EXPECT_EQ(Fixture.Scene.SetStateCalls, 0u);
    EXPECT_TRUE(Fixture.Scene.SimulateCalls.IsEmpty());
    EXPECT_EQ(Fixture.Scene.States[Fixture.Entity].Position, Last.Position);
    const FNetPredictionClient& Prediction = Fixture.Prediction();
    ASSERT_EQ(Prediction.Pending.Num(), 2);
    EXPECT_EQ(Prediction.Pending[0].Command.Seq, AckedSeq + 1);
    EXPECT_EQ(Prediction.Pending[1].Command.Seq, AckedSeq + 2);
    EXPECT_EQ(Prediction.LastAcked, AckedSeq);
    EXPECT_EQ(Prediction.Corrections, 0u);
}

TEST(NetPredictionAck, AStaleAckChangesNothing)
{
    FAckFixture Fixture;
    Fixture.Prediction().LastAcked = AckedSeq;
    const Physics::FCharacterNetState Last = Fixture.Scene.States[Fixture.Entity];

    Fixture.Ack(AckedSeq - 1, DisagreeingHost());
    Fixture.Ack(AckedSeq, DisagreeingHost());

    EXPECT_EQ(Fixture.Scene.SetStateCalls, 0u);
    EXPECT_TRUE(Fixture.Scene.SimulateCalls.IsEmpty());
    EXPECT_EQ(Fixture.Scene.States[Fixture.Entity].Position, Last.Position);
    const FNetPredictionClient& Prediction = Fixture.Prediction();
    EXPECT_EQ(Prediction.Pending.Num(), (int32)MoveCount);
    EXPECT_EQ(Prediction.LastAcked, AckedSeq);
    EXPECT_EQ(Prediction.Corrections, 0u);
}
