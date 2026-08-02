#pragma once
#include "EntitySystem.h"
#include "TimerSystem.generated.h"

namespace Lumina
{
    // Advances the per-world FTimerManager (a ctx singleton) once per frame. Runs first at FrameStart so
    // timer callbacks fire before gameplay systems; no FSystemAccess => serial, since callbacks run Lua.
    REFLECT(System)
    struct STimerSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest))

        static void Update(const FSystemContext& Context) noexcept;
    };
}
