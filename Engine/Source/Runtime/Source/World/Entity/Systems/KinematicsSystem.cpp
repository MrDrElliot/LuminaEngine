#include "RuntimePCH.h"
#include "KinematicsSystem.h"
#include "World/ECS/Registry.h"

#include "Core/Console/ConsoleVariable.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/VelocityComponent.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarKinematicsEnabled("Kinematics.Enabled", true,
        "Resolve a shared per-entity velocity each frame. Off makes every lookup report zero.");

    namespace KinematicsSystemInternal
    {
        constexpr uint32 kKinematicsParallelGrain = 2048;
    }

    void CKinematicsSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RequireUpdate(EUpdateStage::Paused);
    }

    void CKinematicsSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace KinematicsSystemInternal;

        FKinematicsState& KinematicsState = *State;
        KinematicsState.bEnabled = CVarKinematicsEnabled.GetValue();
        if (!KinematicsState.bEnabled)
        {
            return;
        }

        // Only the components whose systems read a velocity, since differencing every transform paid for static scenery and crowds.
        TStampedEntityCache<FEntityKinematics>& Cache = KinematicsState.Entries;
        if (!Cache.BeginPass(SkeletalConsumers.View(), AudioSourceConsumers.View(), ListenerConsumers.View(),
                             CameraConsumers.View(), Velocities.View()))
        {
            return;
        }

        const uint32 Stamp     = Cache.Stamp;
        const float  DeltaTime = Context.GetDeltaTime();
        const float  InvDelta  = DeltaTime > 0.0f ? (1.0f / DeltaTime) : 0.0f;
        FEntityKinematics* Entries = Cache.ByEntityIndex.GetData();

        // An entity in two consumer pools is differenced once, since the second visit sees this pass's stamp.
        const auto Difference = [&](ECS::FEntity Entity, const auto&)
        {
            const STransformComponent* Xform = Transforms.TryGet<STransformComponent>(Entity);
            if (Xform == nullptr)
            {
                return;
            }

            FEntityKinematics& Entry = Entries[Entity.GetIndex()];
            if (Cache.IsFresh(Entry, Entity))
            {
                return;
            }
            if (Entry.Owner != Entity)
            {
                Entry.bHasPrevious = false;
            }

            const FVector3 Location = Xform->GetWorldLocationCached();
            const FVector3 Velocity = (Entry.bHasPrevious && InvDelta > 0.0f)
                ? (Location - Entry.PreviousLocation) * InvDelta
                : FVector3(0.0f);

            Entry.Owner            = Entity;
            Entry.Stamp            = Stamp;
            Entry.LinearVelocity   = Velocity;
            Entry.Speed            = Math::Length(Velocity);
            Entry.PreviousLocation = Location;
            Entry.bHasPrevious     = true;
        };

        // Pools run one after another, so an entity shared by two is never differenced by two workers at once.
        SkeletalConsumers.ParallelForEach(Difference, kKinematicsParallelGrain);
        AudioSourceConsumers.ParallelForEach(Difference, kKinematicsParallelGrain);
        ListenerConsumers.ParallelForEach(Difference, kKinematicsParallelGrain);
        CameraConsumers.ParallelForEach(Difference, kKinematicsParallelGrain);
        Velocities.ParallelForEach(Difference, kKinematicsParallelGrain);

        // Refines only what the difference pass stamped, so a body with no transform is skipped.
        const auto Refine = [&](ECS::FEntity Entity, const FVector3& Velocity)
        {
            if (FEntityKinematics* Entry = Cache.Find(Entity))
            {
                Entry->LinearVelocity = Velocity;
                Entry->Speed          = Math::Length(Velocity);
            }
        };

        // The solver's own value beats a difference, which smears a collision across the whole frame.
        if (Physics::IPhysicsQueries* PhysicsQueries = Context.GetPhysics())
        {
            // The world is not stepping before physics, so its body reads are safe from every worker at once.
            Bodies.ParallelForEach([&](ECS::FEntity Entity, const SRigidBodyComponent&, const STransformComponent&)
            {
                // Most bodies have no consumer, and the stamp check is far cheaper than the scene's status lookup.
                if (Cache.Find(Entity) != nullptr && PhysicsQueries->GetBodyStatus(Entity) == Physics::EPhysicsBodyStatus::Ready)
                {
                    Refine(Entity, PhysicsQueries->GetLinearVelocity(Entity));
                }
            }, kKinematicsParallelGrain);
        }

        // Last, because a character mover owns its velocity outright and CAnimationSystem preferred it.
        Movers.ForEach([&](ECS::FEntity Entity, const SCharacterMovementComponent& Movement, const STransformComponent&)
        {
            Refine(Entity, Movement.Velocity);
        });

        // Mirrored so the reflected component reads the same value in the editor and in script.
        Velocities.ForEach([&](ECS::FEntity Entity, ECS::TMut<SVelocityComponent> OutMut)
        {
            const FEntityKinematics* Entry = Cache.Find(Entity);
            OutMut.SetIfChanged(&SVelocityComponent::Velocity, Entry != nullptr ? Entry->LinearVelocity : FVector3(0.0f));
            OutMut.SetIfChanged(&SVelocityComponent::Speed, Entry != nullptr ? Entry->Speed : 0.0f);
        });
    }
}
