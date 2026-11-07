#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/Components/TransformComponent.h"

#include "Components/NetworkComponent.h"
#include "Components/RepTransformComponent.h"
#include "Core/Serialization/NetArchive.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "Systems/NetworkSystem.h"

using namespace Lumina;
using namespace Lumina::NetworkSystemInternal;

namespace
{
    constexpr uint32 NumOwned   = 3000;
    constexpr uint32 SenderConn = 3;
    constexpr uint32 FirstGuid  = NetGUID_DynamicStart + 1;

    FVector3 PoseOf(uint32 Index)
    {
        return FVector3(static_cast<float>(Index), 2.0f, -static_cast<float>(Index) * 0.5f);
    }

    ECS::FEntity CreateOwned(ECS::FRegistry& Registry, uint32 Index, ENetRole Role)
    {
        const ECS::FEntity Entity = Registry.Create();
        SNetworkComponent& Net = Registry.Emplace<SNetworkComponent>(Entity);
        Net.NetGUID            = SNetGUID{ FirstGuid + Index };
        Net.LocalRole          = Role;
        Net.OwningConnectionId = SenderConn;
        Registry.Emplace<STransformComponent>(Entity).SetLocalLocation(PoseOf(Index));
        return Entity;
    }
}

TEST(NetClientTransformChunk, ALargeOwnedUploadSplitsAndTheServerAppliesEveryRecord)
{
    ECS::FRegistry Client;
    for (uint32 Index = 0; Index < NumOwned; ++Index)
    {
        const ECS::FEntity Entity = CreateOwned(Client, Index, ENetRole::AutonomousProxy);
        Client.Emplace<FRepTransform>(Entity).NetGUID = FirstGuid + Index;
    }

    TVector<uint8> Batch;
    WriteOwnedTransforms(Client, 1.0f, Batch);

    ECS::FRegistry Server;
    FNetWorldState& ServerState = Server.Ctx().Emplace<FNetWorldState>();
    TVector<ECS::FEntity> ServerEntities;
    for (uint32 Index = 0; Index < NumOwned; ++Index)
    {
        ServerEntities.Add(CreateOwned(Server, Index, ENetRole::Authority));
        ServerState.GuidTable.Register(SNetGUID{ FirstGuid + Index }, ServerEntities.Last());
    }

    uint32 TotalCount  = 0;
    uint32 NumMessages = 0;
    Net::ForEachFramedMessage(Batch.GetData(), static_cast<SIZE_T>(Batch.Num()), [&](const uint8* Msg, SIZE_T Size)
    {
        EXPECT_LE(Size, Net::MaxFramedMessageSize);
        FNetArchive Reader(Msg, Size);
        uint8  Type  = 0;
        uint16 Count = 0;
        Reader << Type;
        Reader << Count;
        EXPECT_EQ(Type, static_cast<uint8>(ENetMessage::ClientTransform));
        TotalCount += Count;
        ++NumMessages;
        ApplyClientTransform(Server, ServerState, SenderConn, 1.0, Msg, Size);
    });

    EXPECT_GT(NumMessages, 1u);
    EXPECT_EQ(TotalCount, NumOwned);
    for (uint32 Index = 0; Index < NumOwned; ++Index)
    {
        const FRepTransform* Rep = Server.TryGet<ECS::Read<FRepTransform>>(ServerEntities[Index]);
        ASSERT_NE(Rep, nullptr) << "record " << Index;
        ASSERT_EQ(Rep->Ring.Count, 1);
        EXPECT_NEAR(Rep->Ring.Newest().Pos.x, PoseOf(Index).x, 0.01f);
        EXPECT_NEAR(Rep->Ring.Newest().Pos.z, PoseOf(Index).z, 0.01f);
    }
}
