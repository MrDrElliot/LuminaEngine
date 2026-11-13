#include "HealthComponent.h"
#include "World/ECS/Registry.h"

#include "Core/Math/Math.h"

namespace Lumina
{
    namespace
    {
        SHealthChangedEvent MakeHealthEvent(const SHealthComponent& Health, float Delta, ECS::FEntity Instigator)
        {
            SHealthChangedEvent Event;
            Event.Instigator = Instigator;
            Event.Delta = Delta;
            Event.Health = Health.Health;
            Event.MaxHealth = Health.MaxHealth;
            return Event;
        }

        // Latches bDead before broadcasting so a re-entrant call cannot fire OnDied twice.
        float BroadcastHealthChange(SHealthComponent& Health, float Before, bool bDied, ECS::FEntity Instigator)
        {
            Health.RegenCooldown = Health.RegenDelay;
            Health.bDead = bDied;

            const SHealthChangedEvent Event = MakeHealthEvent(Health, Health.Health - Before, Instigator);
            if (Health.OnHealthChanged.Broadcast(Event) && bDied)
            {
                Health.OnDied.Broadcast(Event);
            }
            return Event.Health;
        }
    }

    float SHealthComponent::ApplyDamage(float Damage, ECS::FEntity Instigator)
    {
        if (bDead || Damage <= 0.0f)
        {
            return Health;
        }

        const float Before = Health;
        Health = Math::Max(0.0f, Health - Damage);
        return BroadcastHealthChange(*this, Before, Health <= 0.0f, Instigator);
    }

    float SHealthComponent::Heal(float Amount, ECS::FEntity Instigator)
    {
        if (bDead || Amount <= 0.0f)
        {
            return Health;
        }

        const float Before = Health;
        Health = Math::Min(MaxHealth, Health + Amount);
        const float After = Health;
        if (After != Before)
        {
            OnHealthChanged.Broadcast(MakeHealthEvent(*this, After - Before, Instigator));
        }
        return After;
    }

    void SHealthComponent::Kill(ECS::FEntity Instigator)
    {
        if (bDead)
        {
            return;
        }

        const float Before = Health;
        Health = 0.0f;
        BroadcastHealthChange(*this, Before, true, Instigator);
    }

    void SHealthComponent::Revive(float NewHealth)
    {
        const float Before = Health;
        bDead = false;
        RegenCooldown = 0.0f;
        Health = NewHealth > 0.0f ? Math::Min(NewHealth, MaxHealth) : MaxHealth;
        OnHealthChanged.Broadcast(MakeHealthEvent(*this, Health - Before, ECS::NullEntity));
    }
}
