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
        Max,
    };
    
    constexpr const char* GUpdateStageNames[] = 
    {
        "FrameStart",
        "PrePhysics",
        "PostPhysics",
        "FrameEnd",
        "Paused"
    };
}
