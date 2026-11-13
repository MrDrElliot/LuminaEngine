#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"

#include "Components/NetworkComponent.h"
#include "Components/RepTransformComponent.h"
#include "Net/NetReplicationGraph.h"

using namespace Lumina;

namespace
{
    constexpr float TickSeconds = 1.0f / 60.0f;

    struct FSendHarness
    {
        ECS::FRegistry           Registry;
        FNetExtract              Extract;
        THashMap<uint32, uint32> OwnerToRecord;
        SDefaultWorldSettings    Settings;
        ECS::FEntity             Entity;

        FSendHarness()
        {
            // No keyframe refresh, so only a real change can make a record due.
            Settings.TransformKeyframeInterval = 0.0f;
            Entity = Registry.Create();
            Registry.Emplace<SNetworkComponent>(Entity).NetGUID = SNetGUID{ NetGUID_DynamicStart + 1 };
            Registry.Emplace<STransformComponent>(Entity);
            Registry.Emplace<FRepTransform>(Entity);
        }

        void Tick(const FVector3& Pos, const FVector3& Scale = FVector3(1.0f))
        {
            STransformComponent& Transform = Registry.Get<ECS::Write<STransformComponent>>(Entity);
            Transform.WorldTransform.SetLocation(Pos);
            Transform.WorldTransform.SetScale(Scale);
            NetGraph::BuildExtract(Registry, Extract, OwnerToRecord);
            ASSERT_EQ(Extract.Num(), 1u);
        }

        // Services Client as the relevancy pass does, returning whether it got a record.
        bool Service(FRelevantEntry& Client, float SinceServiced, bool* bOutScale = nullptr)
        {
            Client.TimeSinceSent += SinceServiced;
            bool bScale = false;
            if (!NetGraph::IsTransformDue(Client, Extract.Seq[0], false, Settings, bScale))
            {
                return false;
            }
            Client.TimeSinceSent = 0.0f;
            Client.SentSeq       = Extract.Seq[0];
            if (bOutScale != nullptr)
            {
                *bOutScale = bScale;
            }
            return true;
        }
    };
}

TEST(NetTransformSend, AClientServicedAfterTheEntityStopsStillGetsTheFinalPose)
{
    FSendHarness H;
    FRelevantEntry NearClient;
    FRelevantEntry MidClient;
    MidClient.Tier = ENetLODTier::Mid;

    H.Tick(FVector3(0.0f));
    EXPECT_TRUE(H.Service(NearClient, TickSeconds));
    EXPECT_TRUE(H.Service(MidClient, TickSeconds));

    H.Tick(FVector3(4.0f, 0.0f, 0.0f));
    H.Tick(FVector3(4.0f, 0.0f, 0.0f));
    EXPECT_TRUE(H.Service(NearClient, 2.0f * TickSeconds));
    EXPECT_EQ(H.Extract.Pos[0], FVector3(4.0f, 0.0f, 0.0f));
    EXPECT_FALSE(H.Service(MidClient, 2.0f * TickSeconds)) << "a Mid client waits for its cadence";

    const float MidPeriod = 1.0f / H.Settings.TierMidRate;
    H.Tick(FVector3(4.0f, 0.0f, 0.0f));
    EXPECT_TRUE(H.Service(MidClient, MidPeriod)) << "the Mid client gets the final pose once its cadence opens";
    EXPECT_FALSE(H.Service(NearClient, MidPeriod)) << "an unchanged entity produces no record";
    EXPECT_FALSE(H.Service(MidClient, MidPeriod));
}

TEST(NetTransformSend, AScaleChangeReachesAClientServicedOnALaterTick)
{
    FSendHarness H;
    FRelevantEntry Client;

    H.Tick(FVector3(0.0f));
    EXPECT_TRUE(H.Service(Client, TickSeconds));

    H.Tick(FVector3(0.0f), FVector3(2.0f));
    H.Tick(FVector3(0.0f), FVector3(2.0f));
    bool bScale = false;
    EXPECT_TRUE(H.Service(Client, 2.0f * TickSeconds, &bScale));
    EXPECT_TRUE(bScale);

    H.Tick(FVector3(0.0f), FVector3(2.0f));
    EXPECT_FALSE(H.Service(Client, TickSeconds));
}
