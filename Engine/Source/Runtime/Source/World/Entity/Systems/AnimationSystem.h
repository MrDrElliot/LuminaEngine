#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "AnimationSystem.generated.h"

namespace Lumina
{
    REFLECT(System)
    struct SAnimationSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics), RequiresUpdate(EUpdateStage::Paused))

    public:

        // Union of both passes' access: writes the skeletal pose + (root motion) transforms + Lua VM
        // (anim notifies); reads the simple-anim / graph / blackboard components. Defined in the .cpp.
        static FSystemAccess Access;

        static void Update(const FSystemContext& SystemContext) noexcept;
    };
}
