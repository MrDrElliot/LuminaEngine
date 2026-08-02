#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "PathFollowSystem.generated.h"

namespace Lumina
{
    // Ticks every SPathFollowComponent: refreshes the cached path (Nav::FindPath), advances the corner cursor,
    // and writes a move direction into the paired controller. In PrePhysics so input lands the same step.
    REFLECT(System)
    struct RUNTIME_API SPathFollowSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics))

    public:

        static FSystemAccess Access;   // disjoint from animation → overlaps it; defined in the .cpp

        static void Update(const FSystemContext& Context) noexcept;
    };
}
