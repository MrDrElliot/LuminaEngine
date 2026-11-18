#pragma once

#include "Platform/GenericPlatform.h"
#include "Object/ObjectMacros.h"
#include "UpdateStage.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EUpdateStage : uint8
    {
        FrameStart,
        PrePhysics,
        PostPhysics,
        FrameEnd,
        Paused,
        FixedUpdate,
        // Stepped by FSimulation at its fixed tick, from player inputs alone, and replayed after a rollback.
        Simulation,
        Max,
    };
    
    constexpr const char* GUpdateStageNames[] = 
    {
        "FrameStart",
        "PrePhysics",
        "PostPhysics",
        "FrameEnd",
        "Paused",
        "FixedUpdate",
        "Simulation",
    };

    static_assert(sizeof(GUpdateStageNames) / sizeof(GUpdateStageNames[0]) == (size_t)EUpdateStage::Max);
}
