#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Net/NetPrediction.h"

using namespace Lumina;

namespace
{
    constexpr uint32 OwnerA = 2;
    constexpr uint32 OwnerB = 3;
    constexpr uint32 LongSessionSeq = 36000;

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
