#pragma once

#include "World/ECS/Registry.h"

#include "EntitySystem.h"
#include "StampedEntityCache.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/VelocityComponent.h"
#include "KinematicsSystem.generated.h"

namespace Lumina
{
    struct FEntityKinematics
    {
        // Stamped so a recycled entity slot never inherits the previous occupant's motion.
        ECS::FEntity    Owner = ECS::NullEntity;

        // Pass this was last written, so an entity that stops being tracked stops reading as tracked.
        uint32          Stamp = 0;

        FVector3        LinearVelocity = FVector3(0.0f);

        float           Speed = 0.0f;

        // Survives across passes, since the fallback path needs a frame of history to difference.
        FVector3        PreviousLocation = FVector3(0.0f);

        bool            bHasPrevious = false;
    };

    // Per-world velocities published as a resource ahead of every consumer.
    struct FKinematicsState : ECS::FResource
    {
        TStampedEntityCache<FEntityKinematics> Entries;

        bool        bEnabled = true;

        NODISCARD FVector3 GetVelocity(ECS::FEntity Entity) const
        {
            const FEntityKinematics* Entry = bEnabled ? Entries.Find(Entity) : nullptr;
            return Entry != nullptr ? Entry->LinearVelocity : FVector3(0.0f);
        }
    };

    // Resolves a world velocity for each entity a consumer reads, from the physics body, the character mover, or a difference.
    REFLECT()
    class RUNTIME_API CKinematicsSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        // PrePhysics, not PostPhysics, so CAnimationSystem reads this the same frame it is written.
        void Configure() override;

        // Every frame, since the stamp and the enabled flag are global state its readers check.
        bool HasWork(const SSystemContext& Context) override { return true; }

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SSkeletalMeshComponent>> SkeletalConsumers;
        ECS::TQuery<ECS::Read<SAudioSourceComponent>> AudioSourceConsumers;
        ECS::TQuery<ECS::Read<SAudioListenerComponent>> ListenerConsumers;
        ECS::TQuery<ECS::Read<SCameraComponent>> CameraConsumers;
        ECS::TQuery<ECS::Write<SVelocityComponent>> Velocities;
        ECS::TQuery<ECS::Read<SRigidBodyComponent>, ECS::Read<STransformComponent>> Bodies;
        ECS::TQuery<ECS::Read<SCharacterMovementComponent>, ECS::Read<STransformComponent>> Movers;
        ECS::TQuery<ECS::Read<STransformComponent>> Transforms;
        TResMut<FKinematicsState> State;
    };

    namespace Kinematics
    {
        // Zero for an untracked entity, which is what an entity that never moves would report anyway.
        NODISCARD FORCEINLINE FVector3 GetVelocity(const FKinematicsState* State, ECS::FEntity Entity)
        {
            return State != nullptr ? State->GetVelocity(Entity) : FVector3(0.0f);
        }
    }
}
