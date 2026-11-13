#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "SaveGame/SaveGameEvents.h"

#include "Components/NetworkComponent.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "Systems/NetworkSystem.h"

using namespace Lumina;

namespace
{
    CStruct* StructAt(const FReplTypeTable& Types, ECS::FComponentTypeID TypeId)
    {
        return Types.ByIndex[Types.TypeIdToIndex[TypeId]].Struct;
    }
}

TEST(NetReplTypeTable, SkipsBoundEventStructsAndStillReplicatesASpawn)
{
    CStruct* Event = SSaveGameRequestEvent::StaticStruct();
    bool bEventBound = false;
    ForEachComponentStruct([&](CStruct* Struct) { bEventBound |= Struct == Event; });
    ASSERT_TRUE(bEventBound);
    ASSERT_EQ(Event->GetComponentOps(), nullptr);

    FNetWorldState ServerState;
    for (const FReplTypeTable::FEntry& Entry : ServerState.ReplTypes.ByIndex)
    {
        EXPECT_NE(Entry.Struct->GetComponentOps(), nullptr);
    }
    EXPECT_EQ(Net::GetProtocolHash(ServerState.ReplTypes), Net::GetProtocolHash(FReplTypeTable()));

    ECS::FRegistry Server;
    const ECS::FEntity Source = Server.Create();
    Server.Emplace<SNetworkComponent>(Source);

    constexpr uint32 Guid = NetGUID_DynamicStart + 3;
    TVector<uint8> Msg;
    NetworkSystemInternal::WriteSpawnMessage(Server, ServerState, Source, Guid, 0, FVector3(0.0f), FQuat::Identity(), FVector3(1.0f), Msg);

    ECS::FRegistry Client;
    FNetWorldState& ClientState = Client.Ctx().Emplace<FNetWorldState>();
    NetworkSystemInternal::ApplySpawnEntity(Client, ClientState, 1, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));

    const ECS::FEntity Spawned = ClientState.GuidTable.Find(SNetGUID{ Guid });
    ASSERT_NE(Spawned, ECS::NullEntity);
    EXPECT_NE(Client.TryGet<ECS::Read<SNetworkComponent>>(Spawned), nullptr);
}

TEST(NetReplTypeTable, AllocateDynamicNeverWrapsToInvalid)
{
    FNetGUIDTable Table;
    Table.NextDynamic = UINT32_MAX;

    const SNetGUID Last = Table.AllocateDynamic();
    EXPECT_TRUE(Last.IsValid());
    EXPECT_EQ(Last.Value, UINT32_MAX);

    const SNetGUID Wrapped = Table.AllocateDynamic();
    EXPECT_TRUE(Wrapped.IsValid());
    EXPECT_NE(Wrapped, SNetGUID::Invalid());
    EXPECT_EQ(Wrapped.Value, NetGUID_DynamicStart);
}

TEST(NetReplTypeTable, ANewSessionSeesATypeReboundAfterTheLastOne)
{
    CStruct* Original = SNetworkComponent::StaticStruct();
    const FComponentOps* Ops = Original->GetComponentOps();
    const ECS::FComponentTypeID TypeId = Ops->GetTypeID();

    CStruct* Rebound = NewObject<CStruct>(nullptr, "SNetReplTypeTableReboundComponent");
    Rebound->AddToRoot();
    Rebound->Size = Original->Size;
    Rebound->Link();

    FNetWorldState First;
    RegisterComponentOps(Rebound, Ops);
    FNetWorldState Second;
    RegisterComponentOps(Original, Ops);
    Rebound->SetComponentOps(nullptr);

    EXPECT_EQ(StructAt(First.ReplTypes, TypeId), Original);
    EXPECT_EQ(StructAt(Second.ReplTypes, TypeId), Rebound);
    EXPECT_NE(Net::GetProtocolHash(First.ReplTypes), Net::GetProtocolHash(Second.ReplTypes));
    EXPECT_EQ(Net::GetProtocolHash(First.ReplTypes), Net::GetProtocolHash(FReplTypeTable()));
}
