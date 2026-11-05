#pragma once

#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/SystemResources.h"
#include "Core/Object/ObjectMacros.h"
#include "Components/NetworkComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Components/RepTransformComponent.h"
#include "NetMovementInterpSystem.generated.h"

namespace Lumina
{
    // The client's render clock behind the server, advanced by SNetMovementInterpSystem from what SNetworkSystem last received.
    struct FNetInterpClock : ECS::FResource
    {
        double LatestServerTime   = 0.0;
        double ServerPlaybackTime = 0.0;
        bool   bNetworked         = false;
        bool   bInitialized       = false;
    };

    // Writes each proxy's interpolated pose from its FRepTransform ring in PostPhysics, apart from the exclusive SNetworkSystem so it can overlap other systems.
    REFLECT()
    class NETWORKINGRUNTIME_API SNetMovementInterpSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<FRepTransform>, ECS::Write<STransformComponent>, ECS::Read<SNetworkComponent>> Proxies;
        TResMut<FNetInterpClock> Clock;
    };
}
