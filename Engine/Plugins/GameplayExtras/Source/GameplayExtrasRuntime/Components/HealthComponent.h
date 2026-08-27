#pragma once

#include "World/ECS/Registry.h"

#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Templates/Optional.h"
#include "HealthComponent.generated.h"

namespace Lumina
{
    // A listener knows which entity's delegate it bound to, so the event names the instigator instead.
    REFLECT()
    struct GAMEPLAYEXTRASRUNTIME_API SHealthChangedEvent
    {
        GENERATED_BODY()

        /** Who caused the change, or null for regeneration and script changes with no source. */
        PROPERTY()
        ECS::FEntity Instigator = ECS::NullEntity;

        /** Signed change actually applied after clamping, negative for damage. */
        PROPERTY()
        float Delta = 0.0f;

        /** Health after the change. */
        PROPERTY()
        float Health = 0.0f;

        PROPERTY()
        float MaxHealth = 0.0f;
    };

    // OnDied fires once per life because bDead latches the edge rather than the value being re-tested.
    REFLECT(Component, Category = "Gameplay")
    struct GAMEPLAYEXTRASRUNTIME_API SHealthComponent
    {
        GENERATED_BODY()

        /** Applies Damage (negative values are ignored) and returns the health left. */
        FUNCTION()
        float ApplyDamage(float Damage, ECS::FEntity Instigator);

        /** Restores health up to MaxHealth and returns the health left. Does nothing while dead. */
        FUNCTION()
        float Heal(float Amount, ECS::FEntity Instigator);

        /** Drops health to zero and fires OnDied, whatever the current value. */
        FUNCTION()
        void Kill(ECS::FEntity Instigator);

        /** Clears the death latch and sets health (0 or less means full). Fires OnHealthChanged, not OnDied. */
        FUNCTION()
        void Revive(float NewHealth);

        FUNCTION()
        bool IsDead() const { return bDead; }

        /** Health as a 0 to 1 fraction of MaxHealth, for a bar. Zero when MaxHealth is not positive. */
        FUNCTION()
        float GetHealthFraction() const { return MaxHealth > 0.0f ? Health / MaxHealth : 0.0f; }

        /** Current health points. Clamped to 0 through MaxHealth by every mutator here. */
        PROPERTY(Editable, Category = "Health")
        float Health = 100.0f;

        /** Maximum health capacity. Health is clamped to this value. */
        PROPERTY(Editable, Category = "Health")
        float MaxHealth = 100.0f;

        /** Points restored per second by SHealthSystem; unset means the entity does not regenerate. */
        PROPERTY(Editable, Category = "Health")
        TOptional<float> RegenPerSecond;

        /** Seconds after taking damage before regeneration resumes. */
        PROPERTY(Editable, Category = "Health", ClampMin = 0.0f)
        float RegenDelay = 3.0f;

        /** Fires on every change, damage and healing alike. */
        PROPERTY()
        TScriptDelegate<SHealthChangedEvent> OnHealthChanged;

        /** Fires once when health reaches zero. */
        PROPERTY()
        TScriptDelegate<SHealthChangedEvent> OnDied;

        /** True once health has reached zero, until Revive. */
        PROPERTY(ReadOnly, Category = "Health")
        bool bDead = false;

        /** Counts down from RegenDelay on damage; regeneration is held off while it is positive. */
        float RegenCooldown = 0.0f;
    };
}
