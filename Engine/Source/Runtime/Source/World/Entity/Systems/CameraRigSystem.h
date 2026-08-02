#pragma once
#include "EntitySystem.h"
#include "CameraRigSystem.generated.h"

namespace Lumina
{
    // Positions cameras from rig components (follow + spring-arm boom) at FrameEnd before SCameraSystem, on
    // settled transforms. Game/simulation only. With both rig components the spring arm wins (processed last).
    REFLECT(System)
    struct SCameraRigSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameEnd, EUpdatePriority::Medium))

        // Writes transforms + the follow/spring-arm rig components; reads the physics scene (spring-arm
        // collision sweep) via the PhysicsQuery resource. Runs before SCameraSystem in the same stage; the
        // shared STransformComponent write keeps that ordering. Defined in the .cpp.
        static FSystemAccess Access;

        static void Update(const FSystemContext& Context) noexcept;
    };
}
