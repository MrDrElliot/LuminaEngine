#pragma once

#include "World/Entity/Systems/EntitySystem.h"
#include "HealthSystem.generated.h"

namespace Lumina
{
    // Drives SHealthComponent::RegenPerSecond and the post-damage cooldown that gates it.
    REFLECT(System)
    struct SHealthSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Low))

        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
    };
}
