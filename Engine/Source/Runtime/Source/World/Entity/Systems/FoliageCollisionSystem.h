#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "FoliageCollisionSystem.generated.h"

namespace Lumina
{
    // Bakes static physics bodies for collision-enabled foliage types; rebakes when instances change.
    REFLECT(System)
    struct RUNTIME_API SFoliageCollisionSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics))

        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
        static void Teardown(const FSystemContext& Context) noexcept;
    };
}
