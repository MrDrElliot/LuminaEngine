#pragma once

#include "World/ECS/Registry.h"

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "SystemContext.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "LifetimeSystem.generated.h"

namespace Lumina
{
    REFLECT()
    class SLifetimeSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override
        {
            RequireUpdate(EUpdateStage::FrameEnd);

            // The destroy defers to the end of the batch, so only the countdown needs declaring.
            Writes<SLifetimeComponent>();
        }
        
        void OnUpdate() override
        {
            const FSystemContext& Context = GetContext();

            LUMINA_PROFILE_SCOPE();
            
            Context.CreateView<SLifetimeComponent>().ForEach([&](ECS::FEntity Entity, SLifetimeComponent& Component)
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
