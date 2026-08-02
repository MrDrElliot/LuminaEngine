#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "SystemContext.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "LifetimeSystem.generated.h"

namespace Lumina
{
    REFLECT(System)
    struct SLifetimeSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameEnd))
        
        static void Update(const FSystemContext& Context) noexcept
        {
            LUMINA_PROFILE_SCOPE();
            
            Context.CreateView<SLifetimeComponent>().each([&](entt::entity Entity, SLifetimeComponent& Component)
            {
                Component.Lifetime -= static_cast<float>(Context.GetDeltaTime());
                if (Component.Lifetime <= 0.0f)
                {
                    Context.Destroy(Entity);
                }
            });
        }
    };
}
