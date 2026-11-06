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
    class RUNTIME_API CLifetimeSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override
        {
            RequireUpdate(EUpdateStage::FrameEnd);
        }

        void OnUpdate(const SSystemContext& Context) override
        {
            const float Dt = Context.GetDeltaTime();
            Expiring.ParallelForEach([&](ECS::FEntity Entity, ECS::TMut<SLifetimeComponent> ComponentMut)
            {
                SLifetimeComponent& Component = ComponentMut.GetMut();
                Component.Lifetime -= Dt;
                if (Component.Lifetime <= 0.0f)
                {
                    Context.Destroy(Entity);
                }
            });
        }

    private:

        ECS::TQuery<ECS::Write<SLifetimeComponent>> Expiring;
    };
}
