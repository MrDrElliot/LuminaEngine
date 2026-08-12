#pragma once

#include "World/Entity/Systems/EntitySystem.h"
#include "PrefabSpawnerSystem.generated.h"

namespace Lumina
{
    REFLECT(System)
    struct SPrefabSpawnerSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Low))
        
        static FSystemAccess Access;

        static void Startup(const FSystemContext& Context) noexcept;
        static void Update(const FSystemContext& Context) noexcept;
        static void Teardown(const FSystemContext& Context) noexcept;
        
    };
}
