#include <gtest/gtest.h>

#include "ENet/ENetTransport.h"
#include "Memory/Memcpy.h"

using namespace Lumina;

namespace
{
    constexpr uint32 BroadcastPeer = 0;

    struct FDelivered
    {
        uint32 Peer = 0;
        uint32 Seq  = 0;

        bool operator==(const FDelivered&) const = default;
    };

    bool Submit(FNetSimQueue& Queue, uint32 Peer, uint32 Seq, uint32 NowMs, int32 LatencyMs, float LossPct = 0.0f, ESendMode Mode = ESendMode::Reliable)
    {
        return Queue.Submit(Peer, 0, Mode, &Seq, sizeof(Seq), NowMs, LatencyMs, LossPct);
    }

    TVector<FDelivered> Flush(FNetSimQueue& Queue, uint32 NowMs)
    {
        TVector<FDelivered> Out;
        Queue.Flush(NowMs, [&](const FNetSimQueue::FPacket& P)
        {
            FDelivered& D = Out.EmplaceGetRef();
            D.Peer = P.Peer;
            Memory::Memcpy(&D.Seq, P.Data.GetData(), sizeof(D.Seq));
        });
        return Out;
    }

    TVector<FDelivered> Expected(std::initializer_list<FDelivered> List)
    {
        return TVector<FDelivered>(List);
    }
}

TEST(ENetSimulator, LatencyKeepsSendOrder)
{
    FNetSimQueue Queue;
    for (uint32 Seq = 0; Seq < 5; ++Seq)
    {
        EXPECT_TRUE(Submit(Queue, 1, Seq, Seq, 100));
    }

    EXPECT_TRUE(Flush(Queue, 99).IsEmpty());
    EXPECT_EQ(Flush(Queue, 102), Expected({ { 1, 0 }, { 1, 1 }, { 1, 2 } }));
    EXPECT_EQ(Flush(Queue, 104), Expected({ { 1, 3 }, { 1, 4 } }));
    EXPECT_TRUE(Queue.IsEmpty());
}

TEST(ENetSimulator, LoweringLatencyNeverOvertakesQueuedPackets)
{
    FNetSimQueue Queue;
    EXPECT_TRUE(Submit(Queue, 1, 0, 0, 100));
    EXPECT_TRUE(Submit(Queue, 1, 1, 10, 20));
    EXPECT_TRUE(Submit(Queue, 1, 2, 10, 0));

    EXPECT_TRUE(Flush(Queue, 99).IsEmpty());
    EXPECT_EQ(Flush(Queue, 100), Expected({ { 1, 0 }, { 1, 1 }, { 1, 2 } }));

    EXPECT_FALSE(Submit(Queue, 1, 3, 101, 0));
    EXPECT_TRUE(Queue.IsEmpty());
}

TEST(ENetSimulator, BroadcastAndPerPeerPacketsInterleaveInSendOrder)
{
    FNetSimQueue Queue;
    EXPECT_TRUE(Submit(Queue, 1, 0, 0, 50));
    EXPECT_TRUE(Submit(Queue, BroadcastPeer, 1, 0, 50));
    EXPECT_TRUE(Submit(Queue, 2, 2, 5, 10));
    EXPECT_TRUE(Submit(Queue, BroadcastPeer, 3, 5, 0));
    EXPECT_TRUE(Submit(Queue, 1, 4, 5, 80));

    EXPECT_EQ(Flush(Queue, 50), Expected({ { 1, 0 }, { BroadcastPeer, 1 }, { 2, 2 }, { BroadcastPeer, 3 } }));
    EXPECT_EQ(Flush(Queue, 85), Expected({ { 1, 4 } }));
}

TEST(ENetSimulator, ReliablePacketsSurviveFullLoss)
{
    FNetSimQueue Queue;
    for (uint32 Seq = 0; Seq < 64; ++Seq)
    {
        EXPECT_FALSE(Submit(Queue, 1, Seq, 0, 0, 100.0f));
    }

    for (uint32 Seq = 0; Seq < 64; ++Seq)
    {
        EXPECT_TRUE(Submit(Queue, 1, Seq, 0, 10, 100.0f));
    }
    EXPECT_EQ(Flush(Queue, 10).Num(), 64);
}

TEST(ENetSimulator, UnreliablePacketsDropAtFullLoss)
{
    FNetSimQueue Queue;
    for (ESendMode Mode : { ESendMode::Unreliable, ESendMode::UnreliableSequenced })
    {
        for (uint32 Seq = 0; Seq < 64; ++Seq)
        {
            EXPECT_TRUE(Submit(Queue, 1, Seq, 0, 0, 100.0f, Mode));
            EXPECT_TRUE(Submit(Queue, BroadcastPeer, Seq, 0, 10, 100.0f, Mode));
        }
    }
    EXPECT_TRUE(Queue.IsEmpty());
    EXPECT_TRUE(Flush(Queue, 1000).IsEmpty());
}

TEST(ENetSimulator, ForceDisconnectedPeerQueuedPacketsAreDiscarded)
{
    FNetSimQueue Queue;
    EXPECT_TRUE(Submit(Queue, 1, 0, 0, 50));
    EXPECT_TRUE(Submit(Queue, 2, 1, 0, 50));
    EXPECT_TRUE(Submit(Queue, 2, 2, 0, 50));
    EXPECT_TRUE(Submit(Queue, 1, 3, 0, 50));

    // Mirrors FENetTransport, which forgets a force-disconnected peer and skips its packets at flush.
    constexpr uint32 DisconnectedPeer = 2;
    TVector<FDelivered> Sent;
    for (const FDelivered& D : Flush(Queue, 50))
    {
        if (D.Peer != DisconnectedPeer)
        {
            Sent.Add(D);
        }
    }
    EXPECT_EQ(Sent, Expected({ { 1, 0 }, { 1, 3 } }));
    EXPECT_TRUE(Queue.IsEmpty());
}
