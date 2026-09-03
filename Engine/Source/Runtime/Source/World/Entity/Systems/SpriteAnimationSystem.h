#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "SpriteAnimationSystem.generated.h"

namespace Lumina
{
    REFLECT(System)
    struct SSpriteAnimationSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics))

    public:

        static FSystemAccess Access;

        static void Update(const FSystemContext& SystemContext) noexcept;
    };
}
