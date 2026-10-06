#pragma once

#include "World/ECS/Registry.h"


#include "Networking/INetworkRuntime.h"

namespace Lumina
{
    // Lumina's own netcode behind the engine's networking interface. Nothing outside this plugin
    // names it; the engine only ever sees INetworkRuntime.
    class FLuminaNetworkRuntime final : public INetworkRuntime
    {
    public:

        static FLuminaNetworkRuntime& Get();

        void Initialize() override;
        void Shutdown() override;
        void Update() override;


        int32 GetConnectedClientCount(const CWorld* World) const override;
        bool TakeClientConnection(CWorld* OldWorld, TUniquePtr<INetworkTransport>& OutTransport,
            FConnectionHandle& OutConnection, uint32& OutLocalPeerId) override;

        void OnWorldEntitiesLoaded(CWorld* World) override;
        void OnEntityAttachmentChanged(CWorld* World, ECS::FEntity Entity) override;

        uint32 GetLocalConnectionId(const CWorld* World) const override;
        void GetConnectionIds(const CWorld* World, TVector<uint32>& OutIds) const override;
        bool IsEntityNetworked(const CWorld* World, ECS::FEntity Entity) const override;
        uint32 GetEntityOwner(const CWorld* World, ECS::FEntity Entity) const override;
        bool SetEntityOwner(CWorld* World, ECS::FEntity Entity, uint32 ConnectionId) override;
        void MarkEntityDirty(CWorld* World, ECS::FEntity Entity) override;
        uint32 EntityToNetId(const CWorld* World, ECS::FEntity Entity) const override;
        ECS::FEntity NetIdToEntity(const CWorld* World, uint32 NetId) const override;
        bool SendScriptRpc(CEntityScript* Script, uint32 RpcId, ERpcTarget Target, uint8 Flags,
            const uint8* Payload, uint32 PayloadSize) override;
        bool IsJoined(const CWorld* World) const override;
        ECS::FEntity FindOwnedPawn(const CWorld* World, uint32 ConnectionId) const override;
        void BindEntityIds(const CWorld* World, FNetArchive& Ar) const override;
    };
}
