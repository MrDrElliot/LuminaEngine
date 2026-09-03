#pragma once
#include "EntitySystem.h"
#include "TweenSystem.generated.h"

namespace Lumina
{
    // Serial by default, since a tween setter or its finished callback can touch anything.
    REFLECT(System)
    struct STweenSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest))

        static void Update(const FSystemContext& Context) noexcept;
    };
}
