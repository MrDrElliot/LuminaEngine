#include "RuntimePCH.h"
#include "ProjectileSystem.h"

#include "Physics/PhysicsScene.h"

namespace Lumina
{
    void SProjectileSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
    }

    void SProjectileSystem::OnUpdate(const SSystemContext& Context)
    {
        const float Dt = Context.GetDeltaTime();
        if (Dt <= 0.0f)
        {
            return;
        }

        Physics::IPhysicsQueries* Queries = Context.GetPhysics();
        const FVector3 Gravity = WorldSettings->Gravity;

        Flyers.ParallelForEach([&](ECS::FEntity Entity, SProjectileComponent& Projectile, STransformComponent& Transform)
        {
            if (Projectile.bHasHit)
            {
                return;
            }

            if (Projectile.GravityScale != 0.0f)
            {
                Projectile.Velocity += Gravity * Projectile.GravityScale * Dt;
            }

            // Projectiles are world-space (unparented), so local position is world position.
            const FVector3 Start = Transform.GetLocalLocation();
            const FVector3 End = Start + Projectile.Velocity * Dt;

            // Sweep this frame's movement and take the nearest hit, ignoring own body and shooter.
            TOptional<SRayResult> Hit;
            if (Queries != nullptr)
            {
                if (Projectile.Radius > 0.0f)
                {
                    SSphereCastSettings Settings;
                    Settings.Start = Start;
                    Settings.End = End;
                    Settings.Radius = Projectile.Radius;
                    Settings.LayerMask = Projectile.CollisionMask;
                    Settings.AddIgnoredEntity(Entity);
                    if (Projectile.Instigator != ECS::NullEntity)
                    {
                        Settings.AddIgnoredEntity(Projectile.Instigator);
                    }

                    Hit = Queries->CastSphereClosest(Settings);
                }
                else
                {
                    SRayCastSettings Settings;
                    Settings.Start = Start;
                    Settings.End = End;
                    Settings.LayerMask = Projectile.CollisionMask;
                    Settings.AddIgnoredEntity(Entity);
                    if (Projectile.Instigator != ECS::NullEntity)
                    {
                        Settings.AddIgnoredEntity(Projectile.Instigator);
                    }

                    Hit = Queries->CastRay(Settings);
                }
            }

            if (Hit.IsSet())
            {
                Transform.SetLocalLocation(Hit->Location);

                SProjectileHitEvent Event;
                Event.Projectile = Entity;
                Event.HitEntity = static_cast<ECS::FEntity>(Hit->Entity);
                Event.Point = Hit->Location;
                Event.Normal = Hit->Normal;
                Event.Damage = Projectile.Damage;

                // Committed ahead of the deferred destroy below, so OnHit still reaches the projectile.
                HitEvents.Send(Event);

                Projectile.bHasHit = true;
                Projectile.Velocity = FVector3(0.0f);
                if (Projectile.bDestroyOnHit)
                {
                    Context.Destroy(Entity);
                }
            }
            else
            {
                Transform.SetLocalLocation(End);
            }
        });
    }
}
