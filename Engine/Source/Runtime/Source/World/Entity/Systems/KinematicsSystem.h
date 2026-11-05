#pragma once

#include "World/ECS/Registry.h"

#include "Containers/Vector.h"
#include "EntitySystem.h"
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
        // Indexed by ECS::FEntity::GetIndex(), sparse, an entry whose Owner mismatches is untracked.
        TVector<FEntityKinematics> ByEntityIndex;

        bool        bEnabled = true;

        uint32      Stamp = 0;

        NODISCARD FVector3 GetVelocity(ECS::FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            if (!bEnabled || Index >= (uint32)ByEntityIndex.Num())
            {
                return FVector3(0.0f);
            }

            const FEntityKinematics& Entry = ByEntityIndex[Index];
            const bool bFresh = Entry.Owner == Entity && Entry.Stamp == Stamp;
            return bFresh ? Entry.LinearVelocity : FVector3(0.0f);
        }
    };

    // Resolves a world velocity for each entity a consumer reads, from the physics body, the character mover, or a difference.
    REFLECT()
    class RUNTIME_API SKinematicsSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        // PrePhysics, not PostPhysics, so SAnimationSystem reads this the same frame it is written.
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
