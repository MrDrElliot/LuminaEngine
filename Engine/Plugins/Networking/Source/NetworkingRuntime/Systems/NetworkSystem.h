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
        NETWORKINGRUNTIME_API void ApplySpawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void ApplyDespawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void OnNetworkComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        NETWORKINGRUNTIME_API void WriteOwnedTransforms(ECS::FRegistry& Registry, float DeltaTime, TVector<uint8>& OutBatch);
        NETWORKINGRUNTIME_API void ApplyClientTransform(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, double ServerNow, const uint8* Data, SIZE_T Size);
    }

    // Per-world networking driver.
    REFLECT()
    class CNetworkSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnExclusiveUpdate(const SExclusiveContext& Context) override;
        bool HasWork(const SSystemContext& Context) override;

    private:

        TResMut<FNetInterpClock> Clock;
    };
}
