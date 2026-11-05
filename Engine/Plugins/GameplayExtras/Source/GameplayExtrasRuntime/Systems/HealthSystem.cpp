#include "HealthSystem.h"
#include "World/ECS/Registry.h"

#include "Components/HealthComponent.h"
#include "Core/Math/Math.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    // Heal broadcasts into script, so it runs deferred to the main thread and the countdown is all this system touches.
    void SHealthSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
    }

    void SHealthSystem::OnUpdate(const SSystemContext& Context)
    {
        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }

        const float DeltaTime = Context.GetDeltaTime();
        if (DeltaTime <= 0.0f)
        {
            return;
        }

        Regenerators.ForEach([&](ECS::FEntity Entity, SHealthComponent& Health)
        {
            if (Health.bDead || !Health.RegenPerSecond.IsSet())
            {
                return;
            }

            if (Health.RegenCooldown > 0.0f)
            {
                Health.RegenCooldown -= DeltaTime;
                return;
            }

            const float Amount = Health.RegenPerSecond.GetValue() * DeltaTime;
            Context.Defer([Entity, Amount](const SExclusiveContext& X)
            {
                ECS::FRegistry& Registry = X.GetRegistry();
                SHealthComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<ECS::Write<SHealthComponent>>(Entity) : nullptr;
                if (Live != nullptr && !Live->bDead)
                {
                    Live->Heal(Amount, ECS::NullEntity);
                }
            });
        });
    }
}
