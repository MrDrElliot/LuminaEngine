#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "BuoyancySystem.generated.h"

namespace Lumina
{
    REFLECT(System)
    struct RUNTIME_API SBuoyancySystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics))
        
        static FSystemAccess Access;

        static void Startup(const FSystemContext& Context) noexcept;
        static void Update(const FSystemContext& Context) noexcept;
    };
}
