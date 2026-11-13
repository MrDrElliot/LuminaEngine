#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"

#include "Components/NetworkComponent.h"
#include "Net/NetWorldState.h"
#include "Systems/NetworkSystem.h"

using namespace Lumina;
using namespace Lumina::NetworkSystemInternal;

namespace
{
    constexpr uint32 ServerConnection = 1;
    constexpr uint32 SpawnedGuid      = NetGUID_DynamicStart + 7;

    struct FClient
    {
        ECS::FRegistry  Registry;
        FNetWorldState& State;

        FClient()
            : State(Registry.Ctx().Emplace<FNetWorldState>())
        {
            Registry.GetSignals<SNetworkComponent>().OnDestroy.Connect<&OnNetworkComponentDestroyed>();
        }

        ~FClient()
        {
            Registry.GetSignals<SNetworkComponent>().OnDestroy.Disconnect<&OnNetworkComponentDestroyed>();
        }

        ECS::FEntity Mapped() const
        {
            return State.GuidTable.Find(SNetGUID{ SpawnedGuid });
        }

        void ReceiveSpawn(bool bTruncated = false)
        {
            ECS::FRegistry Server;
            FNetWorldState ServerState;
            const ECS::FEntity Source = Server.Create();
            Server.Emplace<SNetworkComponent>(Source);
            Server.Emplace<STransformComponent>(Source);

            TVector<uint8> Msg;
            WriteSpawnMessage(Server, ServerState, Source, SpawnedGuid, 0, FVector3(0.0f), FQuat::Identity(), FVector3(1.0f), Msg);
            const SIZE_T Size = static_cast<SIZE_T>(Msg.Num());
            ApplySpawnEntity(Registry, State, ServerConnection, Msg.GetData(), bTruncated ? Size / 2 : Size);
        }

        void ReceiveDespawn()
        {
            TVector<uint8> Msg;
            WriteDespawnMessage(SpawnedGuid, Msg);
            ApplyDespawnEntity(Registry, State, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
        }

        ECS::FEntity CreateNetworked(uint32 Guid)
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<SNetworkComponent>(Entity);
            Registry.Get<ECS::Write<SNetworkComponent>>(Entity).NetGUID = SNetGUID{ Guid };
            return Entity;
        }
    };
}

TEST(NetSpawnLifetime, AWellFormedSpawnRegistersTheEntity)
{
    FClient Client;
    Client.ReceiveSpawn();

    const ECS::FEntity Spawned = Client.Mapped();
    ASSERT_TRUE(Client.Registry.IsValid(Spawned));
    EXPECT_EQ(Client.Registry.Get<ECS::Read<SNetworkComponent>>(Spawned).NetGUID.Value, SpawnedGuid);
    EXPECT_EQ(Client.Registry.NumEntities(), 1);
}

TEST(NetSpawnLifetime, ATruncatedSpawnLeavesNoEntityBehind)
{
    FClient Client;
    Client.ReceiveSpawn(true);

    EXPECT_EQ(Client.Mapped(), ECS::NullEntity);
    EXPECT_EQ(Client.Registry.NumEntities(), 0);
}

TEST(NetSpawnLifetime, ALocallyDestroyedEntityRespawnsAfterTheServerDespawnsIt)
{
    FClient Client;
    Client.ReceiveSpawn();
    const ECS::FEntity First = Client.Mapped();
    ASSERT_TRUE(Client.Registry.IsValid(First));

    ECS::Utils::DestroyEntity(Client.Registry, First);
    EXPECT_EQ(Client.Mapped(), ECS::NullEntity);

    Client.ReceiveDespawn();
    Client.ReceiveSpawn();

    const ECS::FEntity Second = Client.Mapped();
    EXPECT_TRUE(Client.Registry.IsValid(Second));
    EXPECT_NE(Second, First);
}

TEST(NetSpawnLifetime, AMappingToADeadEntityDoesNotBlockASpawn)
{
    FClient Client;
    const ECS::FEntity Dead = Client.Registry.Create();
    Client.Registry.Destroy(Dead);
    Client.State.GuidTable.Register(SNetGUID{ SpawnedGuid }, Dead);

    Client.ReceiveSpawn();

    EXPECT_TRUE(Client.Registry.IsValid(Client.Mapped()));
}

TEST(NetSpawnLifetime, ADespawnClearsAMappingToADeadEntity)
{
    FClient Client;
    const ECS::FEntity Dead = Client.Registry.Create();
    Client.Registry.Destroy(Dead);
    Client.State.GuidTable.Register(SNetGUID{ SpawnedGuid }, Dead);

    Client.ReceiveDespawn();

    EXPECT_EQ(Client.Mapped(), ECS::NullEntity);
}

TEST(NetSpawnLifetime, DestroyingAnEntityLeavesAnotherEntitysMappingAlone)
{
    FClient Client;
    Client.ReceiveSpawn();
    const ECS::FEntity Live = Client.Mapped();

    ECS::Utils::DestroyEntity(Client.Registry, Client.CreateNetworked(SpawnedGuid));

    EXPECT_EQ(Client.Mapped(), Live);
}

TEST(NetSpawnLifetime, ADestroyedChildDropsItsPendingAttach)
{
    FClient Client;
    const ECS::FEntity Child = Client.CreateNetworked(SpawnedGuid + 1);
    Client.State.PendingAttach[static_cast<uint32>(Child.Value)] = SpawnedGuid;

    ECS::Utils::DestroyEntity(Client.Registry, Child);

    EXPECT_FALSE(Client.State.PendingAttach.Contains(static_cast<uint32>(Child.Value)));
}
