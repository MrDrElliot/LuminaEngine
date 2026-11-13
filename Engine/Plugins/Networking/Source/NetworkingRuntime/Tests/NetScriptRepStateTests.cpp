#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableTest.h"
#include "Core/Serialization/NetArchive.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"

#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "Net/ScriptRepState.h"

using namespace Lumina;

namespace
{
    struct FComponentRepPeers
    {
        ECS::FRegistry Server;
        ECS::FRegistry Client;
        FNetWorldState ServerState;
        FNetWorldState ClientState;
        FComponentRepState Diff;
        ECS::FEntity ServerEntity = Server.Create();
        ECS::FEntity ClientEntity = Client.Create();

        void Replicate(bool bBaseline = false)
        {
            TVector<Net::FComponentRepOut> Comps;
            Net::CollectComponentFieldsInto(Server, ServerEntity, ServerState, bBaseline, &Diff, Comps);

            TVector<uint8> Bytes;
            FNetArchive Writer(Bytes);
            Net::BindWriters(Writer, ServerState);
            Net::WriteEntityComponents(Writer, Server, ServerEntity, &Comps);

            FNetArchive Reader(Bytes.GetData(), static_cast<SIZE_T>(Bytes.Num()));
            Net::BindReaders(Reader, ClientState, 1);
            Net::ReadEntityComponents(Reader, Client, ClientState, ClientEntity);
            ASSERT_FALSE(Reader.HasError());
        }
    };
}

TEST(NetScriptRepState, RemovingAScriptKeepsTheNextScriptsDiffBase)
{
    RegisterPendingTypes();
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    CEntityScriptTest* First = Cast<CEntityScriptTest>(EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    CEntityScriptTest* Second = Cast<CEntityScriptTest>(EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);
    First->NetValue = 5;
    Second->NetValue = 1;

    FNetWorldState State;
    FComponentRepState Diff;
    TVector<Net::FScriptRepOut> Out;
    Net::CollectScriptFieldsInto(Registry, Entity, State, false, &Diff, Out);
    ASSERT_EQ(Out.Num(), 2);

    ASSERT_TRUE(EntityScripts::Remove(Registry, Entity, First));
    Second->NetValue = 5;

    Net::CollectScriptFieldsInto(Registry, Entity, State, false, &Diff, Out);
    ASSERT_EQ(Out.Num(), 1) << "the shifted script must not diff against the removed script's last-sent bytes";
    EXPECT_EQ(Out[0].ScriptIndex, 0u);
}

TEST(NetScriptRepState, DetachedScriptsArePrunedFromDiffMaps)
{
    RegisterPendingTypes();
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    CEntityScriptTest* A = Cast<CEntityScriptTest>(EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    CEntityScriptTest* B = Cast<CEntityScriptTest>(EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    CEntityScriptTest* C = Cast<CEntityScriptTest>(EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);
    ASSERT_NE(C, nullptr);

    FNetWorldState State;
    FComponentRepState Diff;
    TVector<Net::FScriptRepOut> Out;
    Net::CollectScriptFieldsInto(Registry, Entity, State, true, &Diff, Out);
    ASSERT_EQ(Diff.ScriptLastSent.Num(), 3);

    // Simulates a rate-limited field having sent once, since the test script has none.
    for (const auto& Pair : Diff.ScriptLastSent)
    {
        Diff.ScriptFieldSentTime[Pair.Key] = TVector<double>{ 0.0 };
    }
    ASSERT_EQ(Diff.ScriptFieldSentTime.Num(), 3);

    ASSERT_TRUE(EntityScripts::Remove(Registry, Entity, A));
    Net::CollectScriptFieldsInto(Registry, Entity, State, false, &Diff, Out);
    EXPECT_EQ(Diff.ScriptLastSent.Num(), 2);
    EXPECT_EQ(Diff.ScriptFieldSentTime.Num(), 2);

    ASSERT_TRUE(EntityScripts::Remove(Registry, Entity, B));
    Net::CollectScriptFieldsInto(Registry, Entity, State, false, &Diff, Out);
    EXPECT_EQ(Diff.ScriptLastSent.Num(), 1);
    EXPECT_EQ(Diff.ScriptFieldSentTime.Num(), 1);
}

TEST(NetScriptRepState, RemovingAComponentRemovesItOnTheClient)
{
    RegisterPendingTypes();
    FComponentRepPeers Peers;
    Peers.Server.Emplace<SSimpleAnimationComponent>(Peers.ServerEntity).PlaybackSpeed = 2.0f;
    Peers.Replicate(true);
    ASSERT_NE(Peers.Client.TryGet<ECS::Read<SSimpleAnimationComponent>>(Peers.ClientEntity), nullptr);

    Peers.Server.Remove<SSimpleAnimationComponent>(Peers.ServerEntity);
    Peers.Replicate();
    EXPECT_EQ(Peers.Client.TryGet<ECS::Read<SSimpleAnimationComponent>>(Peers.ClientEntity), nullptr);
    EXPECT_EQ(Peers.Diff.LastSent.Num(), 0);
}

TEST(NetScriptRepState, ReaddingARemovedComponentResendsEveryField)
{
    RegisterPendingTypes();
    FComponentRepPeers Peers;
    Peers.Server.Emplace<SSimpleAnimationComponent>(Peers.ServerEntity).PlaybackSpeed = 2.0f;
    Peers.Replicate(true);

    Peers.Server.Remove<SSimpleAnimationComponent>(Peers.ServerEntity);
    Peers.Replicate();
    ASSERT_EQ(Peers.Client.TryGet<ECS::Read<SSimpleAnimationComponent>>(Peers.ClientEntity), nullptr);

    Peers.Server.Emplace<SSimpleAnimationComponent>(Peers.ServerEntity).PlaybackSpeed = 2.0f;
    Peers.Replicate();
    const SSimpleAnimationComponent* Readded = Peers.Client.TryGet<ECS::Read<SSimpleAnimationComponent>>(Peers.ClientEntity);
    ASSERT_NE(Readded, nullptr);
    EXPECT_EQ(Readded->PlaybackSpeed, 2.0f) << "the re-added component must not diff against its pre-removal snapshot";
}
