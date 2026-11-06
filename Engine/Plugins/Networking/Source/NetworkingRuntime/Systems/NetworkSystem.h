#pragma once
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "World/ECS/Registry.h"
#include "Core/Math/Math.h"
#include "Systems/NetMovementInterpSystem.h"
#include "NetworkSystem.generated.h"

namespace Lumina
{
    struct FNetWorldState;

    // Exported so the replication tests can drive spawn and despawn without a transport.
    namespace NetworkSystemInternal
    {
        NETWORKINGRUNTIME_API void WriteSpawnMessage(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity, uint32 Guid, uint32 Owner,
            const FVector3& Pos, const FQuat& Rot, const FVector3& Scale, TVector<uint8>& OutMsg);
        NETWORKINGRUNTIME_API void WriteDespawnMessage(uint32 Guid, TVector<uint8>& OutMsg);
        NETWORKINGRUNTIME_API void ApplySpawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void ApplyDespawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void OnNetworkComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);
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
