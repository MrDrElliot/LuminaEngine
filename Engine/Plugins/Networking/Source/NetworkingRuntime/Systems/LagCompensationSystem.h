#pragma once

#include "Components/LagCompensationComponent.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Net/LagCompensation.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/SystemResources.h"
#include "LagCompensationSystem.generated.h"

namespace Lumina
{
    class CWorld;

    // Records every lag compensated hitbox at the end of each host frame, the same time the transform snapshots are stamped with.
    REFLECT(HostOnly)
    class NETWORKINGRUNTIME_API CLagCompensationSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        bool HasWork(const SSystemContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SLagCompensationComponent>, ECS::Read<STransformComponent>> Hitboxes;
        TResMut<FLagCompensationHistory> History;
    };

    // Lag compensated hit tests, a view time from the shooter's client and the rewind on the host.
    REFLECT()
    class NETWORKINGRUNTIME_API CLagCompensationLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()
    public:

        // Client side, the host time Target was drawn at on this screen, to send along with a shot at it. Null Target gives the usual delay.
        FUNCTION()
        static double GetViewTime(CWorld* World, ECS::FEntity Target);

        // Host side, a ray or swept sphere tested against every hitbox where it was at ViewTime, clamped to the rewind window.
        FUNCTION()
        static SLagCompensatedHit Raycast(CWorld* World, double ViewTime, FVector3 Origin, FVector3 Direction, float MaxDistance, float Thickness, ECS::FEntity Ignore);

        // Host side, every entity whose hitbox a sphere touched at ViewTime.
        FUNCTION()
        static TVector<ECS::FEntity> OverlapSphere(CWorld* World, double ViewTime, FVector3 Center, float Radius);
    };
}
