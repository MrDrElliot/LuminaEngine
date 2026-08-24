#include "HealthSystem.h"

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
        TVector<entt::entity> Regenerating;
        {
            auto View = Context.CreateView<SHealthComponent>();
            for (entt::entity Entity : View)
            {
                const SHealthComponent& Health = View.get<SHealthComponent>(Entity);
                if (!Health.bDead && Health.RegenPerSecond.IsSet())
                {
                    Regenerating.push_back(Entity);
                }
            }
        }

        FEntityRegistry& Registry = Context.GetRegistry();
        for (const entt::entity Entity : Regenerating)
        {
            SHealthComponent* Health = Registry.try_get<SHealthComponent>(Entity);
            if (Health == nullptr || Health->bDead || !Health->RegenPerSecond.IsSet())
            {
                continue;
            }

            if (Health->RegenCooldown > 0.0f)
            {
                Health->RegenCooldown -= DeltaTime;
                continue;
            }

            Health->Heal(Health->RegenPerSecond.GetValue() * DeltaTime, entt::null);
        }
    }
}
