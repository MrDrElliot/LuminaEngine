#pragma once
#include "EntitySystem.h"
#include "NetworkSystem.generated.h"

namespace Lumina
{
    // Per-world networking driver.
    REFLECT(System)
    struct SNetworkSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest))

        static void Update(const FSystemContext& Context) noexcept;
    };
}
