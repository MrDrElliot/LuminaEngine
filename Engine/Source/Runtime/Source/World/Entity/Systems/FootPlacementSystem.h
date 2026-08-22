#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "FootPlacementSystem.generated.h"

namespace Lumina
{
    // Traces the ground under every foot and publishes the result as animation graph parameters.
    REFLECT(System)
    struct RUNTIME_API SFootPlacementSystem
    {
        GENERATED_BODY()

        // Ahead of the animation system, so the parameters are current when the graph evaluates.
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::High))

        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
    };
}
