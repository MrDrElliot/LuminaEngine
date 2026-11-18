#include "RuntimePCH.h"
#include "ProjectileSystem.h"
#include "MovementComponentSystem.h"

#include "Physics/PhysicsScene.h"

namespace Lumina
{
    void CProjectileSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RunsAfter<CMovementComponentSystem>();
    }

    void CProjectileSystem::OnUpdate(const SSystemContext& Context)
    {
        const float Dt = Context.GetDeltaTime();
        if (Dt <= 0.0f)
        {
            return;
        }

        Physics::IPhysicsQueries* Queries = Context.GetPhysics();
        const FVector3 Gravity = WorldSettings->Gravity;

        auto Step = [&](ECS::FEntity Entity, ECS::TMut<SProjectileComponent> ProjectileMut, ECS::TMut<STransformComponent> TransformMut, bool bDecides)
        {
            if (ProjectileMut.Get().bHasHit)
            {
                return;
            }

            STransformComponent& Transform = TransformMut.GetMut();
            SProjectileComponent& Projectile = ProjectileMut.GetMut();
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
                Projectile.bHasHit = true;
                Projectile.Velocity = FVector3(0.0f);

                // A mirror rests where it struck and waits for the host's despawn, since the host decides what it hit.
                if (!bDecides)
                {
                    return;
                }

                SProjectileHitEvent Event;
                Event.Projectile = Entity;
                Event.HitEntity = static_cast<ECS::FEntity>(Hit->Entity);
                Event.Point = Hit->Location;
                Event.Normal = Hit->Normal;
                Event.Damage = Projectile.Damage;

                // Committed ahead of the deferred destroy below, so OnHit still reaches the projectile.
                HitEvents.Send(Event);

                if (Projectile.bDestroyOnHit)
                {
                    Context.Destroy(Entity);
                }
            }
            else
            {
                Transform.SetLocalLocation(End);
            }
        };

        Flyers.ParallelForEach([&](ECS::FEntity Entity, ECS::TMut<SProjectileComponent> Projectile, ECS::TMut<STransformComponent> Transform)
        {
            Step(Entity, Projectile, Transform, true);
        });
        Mirrors.ParallelForEach([&](ECS::FEntity Entity, ECS::TMut<SProjectileComponent> Projectile, ECS::TMut<STransformComponent> Transform)
        {
            Step(Entity, Projectile, Transform, false);
        });
    }
}
