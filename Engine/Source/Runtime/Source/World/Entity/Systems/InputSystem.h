#pragma once
#include "EntitySystem.h"
#include "InputSystem.generated.h"

namespace Lumina
{
    REFLECT(System)
    struct SInputSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest))
        
        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
    };
}
