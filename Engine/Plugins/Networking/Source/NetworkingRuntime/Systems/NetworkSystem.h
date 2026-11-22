#pragma once
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "World/ECS/Registry.h"
#include "Core/Math/Math.h"
#include "Systems/NetMovementInterpSystem.h"
#include "Net/NetReplicationGraph.h"
#include "NetworkSystem.generated.h"

namespace Lumina
{
    struct FNetWorldState;
    class FNetArchive;
    class CWorld;

    // Exported so the replication tests can drive spawn and despawn without a transport.
    namespace NetworkSystemInternal
    {
        // The wire encoding is chosen by tier at write time and mirrored by tier at read time.
        struct FTransformSendRecord
        {
            uint32      Guid = 0;
            ENetLODTier Tier = ENetLODTier::Near;
            FVector3    Pos;
            FQuat       Rot;
            bool        bScale = false;
            FVector3    Scale  = FVector3(1.0f);
            bool        bPitch = false;
            int8        Pitch  = 0;
        };

        NETWORKINGRUNTIME_API void WriteTransformRecord(FNetArchive& Writer, const FTransformSendRecord& R, const FVector3& Anchor);
        NETWORKINGRUNTIME_API bool ReadTransformIntoRing(ECS::FRegistry& Registry, FNetWorldState& State, FNetArchive& Reader, double SampleTime, bool bSkipAutonomous,
            uint32 OwnerGate, const FVector3& Anchor);
        NETWORKINGRUNTIME_API void WriteSpawnMessage(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity, uint32 Guid, uint32 Owner,
            const FVector3& Pos, const FQuat& Rot, const FVector3& Scale, TVector<uint8>& OutMsg);
        NETWORKINGRUNTIME_API void WriteDespawnMessage(uint32 Guid, TVector<uint8>& OutMsg);
        // Host, gives new networked entities their ids, and the children that inherit their networking theirs in the same tick.
        NETWORKINGRUNTIME_API void MaintainDynamicLifetime(ECS::FRegistry& Registry, FNetWorldState& State);

        // A lost packet's entity as it is now, with every replicated component it has lost, written without moving the shared diff baseline.
        NETWORKINGRUNTIME_API void WritePropertyResync(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity, uint32 Guid, TVector<uint8>& OutMsg);

        // Client, applies a numbered property packet and acknowledges it only when every update in it landed.
        NETWORKINGRUNTIME_API void ApplyPropertyPacket(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size);

        // Gives every descendant of Parent without a network component of its own a copy of its parent's, adding each one to OutAdded.
        NETWORKINGRUNTIME_API void InheritNetworking(ECS::FRegistry& Registry, ECS::FEntity Parent, TVector<ECS::FEntity>* OutAdded = nullptr);

        // Client, records StandIn as waiting on the host's copy and returns the key to send with the request.
        NETWORKINGRUNTIME_API uint32 BeginPredictedSpawn(FNetWorldState& State, ECS::FEntity StandIn, bool bKeepStandIn, double Expire);

        // Client, pairs a spawn the host made for this peer with the stand-in waiting on it.
        NETWORKINGRUNTIME_API void ResolvePredictedSpawn(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity);

        // Client, gives up on stand-ins whose host copy never came, destroying them.
        NETWORKINGRUNTIME_API void ExpirePredictedSpawns(ECS::FRegistry& Registry, FNetWorldState& State, double Now);

        // A client is AutonomousProxy for what it owns and SimulatedProxy for the rest, every tick, and tags what it only mirrors.
        NETWORKINGRUNTIME_API void RefreshNetRoles(ECS::FRegistry& Registry, const FNetWorldState& State, bool bServer);
        NETWORKINGRUNTIME_API void ApplySpawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void ApplyDespawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size);

        // Host, marks every networked entity dirty whose replicated component fields were written since the last scan.
        NETWORKINGRUNTIME_API void MarkReplicatedChanges(ECS::FRegistry& Registry, FNetWorldState& State);

        // Host, ends networking for the entities torn off since the last send, owing each client that held one a message to keep its copy.
        NETWORKINGRUNTIME_API void ProcessTearOffs(CWorld* World, ECS::FRegistry& Registry, FNetWorldState& State);
        NETWORKINGRUNTIME_API void ApplyTearOff(ECS::FRegistry& Registry, CWorld* World, FNetWorldState& State, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void OnNetworkComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        NETWORKINGRUNTIME_API void WriteOwnedTransforms(ECS::FRegistry& Registry, float DeltaTime, TVector<uint8>& OutBatch);
        NETWORKINGRUNTIME_API void ApplyClientTransform(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, double ServerNow, const uint8* Data, SIZE_T Size);
    }

    // Per-world networking driver.
    REFLECT(Networked)
    class CNetworkSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        TResMut<FNetInterpClock> Clock;
    };
}
