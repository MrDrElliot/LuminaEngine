#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/ProjectileComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include "ProjectileSystem.generated.h"

namespace Lumina
{

    // Sweeps every projectile forward in PrePhysics, reports the first hit, and despawns on hit.
    REFLECT()
    class RUNTIME_API CProjectileSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SProjectileComponent>, ECS::Write<STransformComponent>, ECS::Without<SNetProxyTag>> Flyers;

        // A host's projectile whose spawn alone replicated, flown here along the same path while the host judges what it hits.
        ECS::TQuery<ECS::Write<SProjectileComponent>, ECS::Write<STransformComponent>, ECS::With<SNetProxyTag>, ECS::Without<SNetDrivenTransformTag>> Mirrors;
        ECS::TEventWriter<SProjectileHitEvent> HitEvents;
        TRes<SWorldSettingsState> WorldSettings;
    };
}
