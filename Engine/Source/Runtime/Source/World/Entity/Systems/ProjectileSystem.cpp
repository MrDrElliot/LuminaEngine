#include "RuntimePCH.h"
#include "ProjectileSystem.h"
#include "World/ECS/Registry.h"

#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/Components/ProjectileComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SystemContext.h"

namespace Lumina
{
    void SProjectileSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        const float Dt = (float)Context.GetDeltaTime();
        if (Dt <= 0.0f)
        {
            return;
        }

        Physics::IPhysicsScene* Scene = Context.GetPhysicsScene();

        // World gravity vector, for arced projectiles (GravityScale != 0).
        FVector3 Gravity(0.0f);
        if (CWorld* CW = Context.GetRegistry().Ctx().Get<CWorld*>())
        {
            const SDefaultWorldSettings& Settings = CW->GetDefaultWorldSettings();
            Gravity = Settings.GravityDirection * (9.81f * Settings.GravityScale);
        }

        auto View = Context.CreateView<SProjectileComponent, STransformComponent>();
        View.ForEach([&](ECS::FEntity Entity, SProjectileComponent& Projectile, STransformComponent& Transform)
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
            if (Scene != nullptr)
            {
                const uint32 SelfBody = Context.GetEntityBodyID(Entity);
                const uint32 InstigatorBody = (Projectile.Instigator != ECS::NullEntity)
                    ? Context.GetEntityBodyID(Projectile.Instigator) : ~0u;

                if (Projectile.Radius > 0.0f)
                {
                    SSphereCastSettings Settings;
                    Settings.Start = Start;
                    Settings.End = End;
                    Settings.Radius = Projectile.Radius;
                    Settings.LayerMask = Projectile.CollisionMask;
                    if (SelfBody != ~0u)       { Settings.AddIgnoredBody(SelfBody); }
                    if (InstigatorBody != ~0u) { Settings.AddIgnoredBody(InstigatorBody); }

                    Hit = Scene->CastSphereClosest(Settings);
                }
                else
                {
                    SRayCastSettings Settings;
                    Settings.Start = Start;
                    Settings.End = End;
                    Settings.LayerMask = Projectile.CollisionMask;
                    if (SelfBody != ~0u)       { Settings.AddIgnoredBody(SelfBody); }
                    if (InstigatorBody != ~0u) { Settings.AddIgnoredBody(InstigatorBody); }

                    Hit = Scene->CastRay(Settings);
                }
            }

            if (Hit.has_value())
            {
                Transform.SetLocalLocation(Hit->Location);

                SProjectileHitEvent Event;
                Event.Projectile = Entity;
                Event.HitEntity = static_cast<ECS::FEntity>(Hit->Entity);
                Event.Point = Hit->Location;
                Event.Normal = Hit->Normal;
                Event.Damage = Projectile.Damage;
                Projectile.OnHit.Broadcast(Event);

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
