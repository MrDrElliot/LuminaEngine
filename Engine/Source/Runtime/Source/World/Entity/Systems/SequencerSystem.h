#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "SequencerSystem.generated.h"

namespace Lumina
{
    // Advances every SSequencePlayerComponent and writes its tracks into the world. Runs on PrePhysics at
    // high priority: a sequence poses entities, and animation, physics and the camera all read those poses
    // later in the same frame.
    REFLECT(System)
    struct SSequencerSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::High))

    public:

        static void Update(const FSystemContext& SystemContext) noexcept;
    };
}
