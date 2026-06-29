#pragma once
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Physics/PhysicsTypes.h"
#include "World/Entity/EntityHandle.h"
#include "ProjectileComponent.generated.h"

namespace Lumina
{
    // Broadcast by SProjectileComponent::OnHit when a projectile strikes something. Blittable so C#
    // listeners receive it by value.
    REFLECT()
    struct RUNTIME_API SProjectileHitEvent
    {
        GENERATED_BODY()

        PROPERTY(Script)
        FEntity Projectile = entt::null;

        PROPERTY(Script)
        FEntity HitEntity = entt::null;

        PROPERTY(Script)
        FVector3 Point;

        PROPERTY(Script)
        FVector3 Normal;

        PROPERTY(Script)
        float Damage = 0.0f;
    };

    // A moving projectile. SProjectileSystem sweeps it forward each frame (continuous, so it can't tunnel
    // through thin geometry), reports the first hit through OnHit, and optionally destroys it. Spawn one
    // with CWorld::SpawnProjectile, or add this component to any entity.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SProjectileComponent
    {
        GENERATED_BODY()

        /** World-space velocity in meters per second. */
        PROPERTY(Script, Editable, Category = "Projectile")
        FVector3 Velocity;

        /** Multiplier on world gravity. 0 = travels in a straight line. */
        PROPERTY(Script, Editable, Category = "Projectile")
        float GravityScale = 0.0f;

        /** Sweep radius in meters. 0 = a thin ray; larger fits a bigger/faster projectile. */
        PROPERTY(Script, Editable, Category = "Projectile", ClampMin = 0.0f)
        float Radius = 0.0f;

        /** Carried in the hit event; gameplay decides how to apply it. */
        PROPERTY(Script, Editable, Category = "Projectile")
        float Damage = 0.0f;

        /** Collision layers the projectile can hit. */
        PROPERTY(Script, Editable, Category = "Projectile")
        ECollisionProfiles CollisionMask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;

        /** Destroy the projectile entity on its first hit. */
        PROPERTY(Script, Editable, Category = "Projectile")
        bool bDestroyOnHit = true;

        /** Entity that fired this projectile; the sweep ignores it so it never hits its own shooter. */
        PROPERTY(Script, Editable, Entity, Category = "Projectile")
        FEntity Instigator = entt::null;

        /** Fired once when the projectile hits something. */
        PROPERTY(Script)
        TScriptDelegate<SProjectileHitEvent> OnHit;

        // Transient: set once a hit registers, so the system stops sweeping a resting projectile.
        bool bHasHit = false;
    };
}
