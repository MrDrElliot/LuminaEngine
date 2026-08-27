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
    }

    float SHealthComponent::ApplyDamage(float Damage, ECS::FEntity Instigator)
    {
        if (bDead || Damage <= 0.0f)
        {
            return Health;
        }

        const float Before = Health;
        Health = Math::Max(0.0f, Health - Damage);
        RegenCooldown = RegenDelay;

        const SHealthChangedEvent Event = MakeHealthEvent(*this, Health - Before, Instigator);
        OnHealthChanged.Broadcast(Event);

        // Latched before the broadcast so a listener that heals from OnDied is not treated as still alive.
        if (Health <= 0.0f)
        {
            bDead = true;
            OnDied.Broadcast(Event);
        }
        return Health;
    }

    float SHealthComponent::Heal(float Amount, ECS::FEntity Instigator)
    {
        if (bDead || Amount <= 0.0f)
        {
            return Health;
        }

        const float Before = Health;
        Health = Math::Min(MaxHealth, Health + Amount);
        if (Health != Before)
        {
            OnHealthChanged.Broadcast(MakeHealthEvent(*this, Health - Before, Instigator));
        }
        return Health;
    }

    void SHealthComponent::Kill(ECS::FEntity Instigator)
    {
        if (bDead)
        {
            return;
        }

        const float Before = Health;
        Health = 0.0f;
        bDead = true;
        RegenCooldown = RegenDelay;

        const SHealthChangedEvent Event = MakeHealthEvent(*this, -Before, Instigator);
        OnHealthChanged.Broadcast(Event);
        OnDied.Broadcast(Event);
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
