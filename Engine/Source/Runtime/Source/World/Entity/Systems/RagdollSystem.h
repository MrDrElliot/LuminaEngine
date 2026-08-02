#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "RagdollSystem.generated.h"

namespace Lumina
{
    // Bridges SRagdollComponent to the physics scene: on PrePhysics it creates/destroys the Jolt ragdoll
    // (seeded from the current animation pose, so it must run after SAnimationSystem); on PostPhysics it
    // reads the simulated bodies back into the skeletal mesh's bone transforms.
    REFLECT(System)
    struct SRagdollSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Low),
                      RequiresUpdate(EUpdateStage::PostPhysics))

    public:

        static void Update(const FSystemContext& SystemContext) noexcept;
    };
}
