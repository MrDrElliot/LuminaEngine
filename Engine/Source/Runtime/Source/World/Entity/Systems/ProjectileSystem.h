#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "ProjectileSystem.generated.h"

namespace Lumina
{
    // Sweeps every SProjectileComponent forward each frame, reports the first hit, and despawns on hit or
    // lifetime expiry. Runs in PrePhysics so movement lands before the physics step and rendering.
    REFLECT(System)
    struct RUNTIME_API SProjectileSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics))

        static void Update(const FSystemContext& Context) noexcept;
    };
}
