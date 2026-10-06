#include "HealthSystem.h"
#include "World/ECS/Registry.h"

#include "Components/HealthComponent.h"
#include "Core/Math/Math.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    // Heal broadcasts into script, so it runs at the sync point and the countdown is all this system touches.
    void SHealthSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Low);
        Writes<SHealthComponent>();
    }

    void SHealthSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }

        const float DeltaTime = (float)Context.GetDeltaTime();
        if (DeltaTime <= 0.0f)
        {
            return;
        }

        ECS::FCommandBus& Bus = Context.GetCommandBus();
        auto View = Context.CreateView<SHealthComponent>();
        for (ECS::FEntity Entity : View)
        {
            SHealthComponent& Health = View.Get<SHealthComponent>(Entity);
            if (Health.bDead || !Health.RegenPerSecond.IsSet())
            {
                continue;
            }

            if (Health.RegenCooldown > 0.0f)
            {
                Health.RegenCooldown -= DeltaTime;
                continue;
            }

            const float Amount = Health.RegenPerSecond.GetValue() * DeltaTime;
            Bus.Enqueue([Entity, Amount](ECS::FRegistry& Registry)
            {
                SHealthComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<SHealthComponent>(Entity) : nullptr;
                if (Live != nullptr && !Live->bDead)
                {
                    Live->Heal(Amount, ECS::NullEntity);
                }
            });
        }
    }
}
