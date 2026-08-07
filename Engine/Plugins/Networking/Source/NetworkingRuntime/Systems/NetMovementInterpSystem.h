#pragma once

#include "World/Entity/Systems/EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "NetMovementInterpSystem.generated.h"

namespace Lumina
{
    // Bulk transform-replication update pass. Reads each proxy's FRepTransform sample ring and writes the
    // interpolated/extrapolated pose back into STransformComponent, in parallel. Runs in PostPhysics, so it
    // sees the step that just ran and Kinematic proxy bodies follow the next one. Separate
    // from SNetworkSystem because that system is exclusive (transport pump, Lua, structural spawn/destroy);
    // this one declares a disjoint write set so the scheduler can overlap it.
    REFLECT(System)
    struct NETWORKINGRUNTIME_API SNetMovementInterpSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PostPhysics, EUpdatePriority::High))

    public:

        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
    };
}
