#include "HealthSystem.h"
#include "World/ECS/Registry.h"

#include "Components/HealthComponent.h"
#include "Core/Math/Math.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    // Heal broadcasts, and a listener may do anything, so this cannot claim a narrow component set.
    FSystemAccess SHealthSystem::Access = FSystemAccess::Exclusive();

    void SHealthSystem::Update(const FSystemContext& Context) noexcept
    {
        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }

        const float DeltaTime = (float)Context.GetDeltaTime();
        if (DeltaTime <= 0.0f)
        {
            return;
        }

        // Snapshotted because Heal broadcasts into script code that can add or destroy entities.
        TVector<ECS::FEntity> Regenerating;
        {
            auto View = Context.CreateView<SHealthComponent>();
            for (ECS::FEntity Entity : View)
            {
                const SHealthComponent& Health = View.Get<SHealthComponent>(Entity);
                if (!Health.bDead && Health.RegenPerSecond.IsSet())
                {
                    Regenerating.push_back(Entity);
                }
            }
        }

        ECS::FRegistry& Registry = Context.GetRegistry();
        for (const ECS::FEntity Entity : Regenerating)
        {
            SHealthComponent* Health = Registry.TryGet<SHealthComponent>(Entity);
            if (Health == nullptr || Health->bDead || !Health->RegenPerSecond.IsSet())
            {
                continue;
            }

            if (Health->RegenCooldown > 0.0f)
            {
                Health->RegenCooldown -= DeltaTime;
                continue;
            }

            Health->Heal(Health->RegenPerSecond.GetValue() * DeltaTime, ECS::NullEntity);
        }
    }
}
