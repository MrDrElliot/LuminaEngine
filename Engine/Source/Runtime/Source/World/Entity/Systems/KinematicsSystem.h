#pragma once

#include "World/ECS/Registry.h"

#include "Containers/Vector.h"
#include "EntitySystem.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "KinematicsSystem.generated.h"

namespace Lumina
{
    class CWorld;

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

    // Per-world velocities published into the registry context ahead of every consumer.
    struct FKinematicsState
    {
        // Indexed by ECS::FEntity::GetIndex(), sparse, an entry whose Owner mismatches is untracked.
        TVector<FEntityKinematics> ByEntityIndex;

        bool        bEnabled = true;

        uint32      Stamp = 0;

        NODISCARD FVector3 GetVelocity(ECS::FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            if (!bEnabled || Index >= (uint32)ByEntityIndex.size())
            {
                return FVector3(0.0f);
            }

            const FEntityKinematics& Entry = ByEntityIndex[Index];
            const bool bFresh = Entry.Owner == Entity && Entry.Stamp == Stamp;
            return bFresh ? Entry.LinearVelocity : FVector3(0.0f);
        }
    };

    // Resolves one world velocity per entity from the physics body, the character mover, or a difference.
    REFLECT(System)
    struct RUNTIME_API SKinematicsSystem
    {
        GENERATED_BODY()
        // PrePhysics, not PostPhysics, so SAnimationSystem reads this the same frame it is written.
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Highest),
                      RequiresUpdate(EUpdateStage::Paused,     EUpdatePriority::Highest))

    public:

        static FSystemAccess Access;

        static void Startup(const FSystemContext& Context) noexcept;
        static void Update (const FSystemContext& Context) noexcept;
    };

    namespace Kinematics
    {
        RUNTIME_API const FKinematicsState* GetState(const FSystemContext& Context);
        RUNTIME_API const FKinematicsState* GetState(CWorld* World);

        // Zero for an untracked entity, which is what an entity that never moves would report anyway.
        NODISCARD FORCEINLINE FVector3 GetVelocity(const FKinematicsState* State, ECS::FEntity Entity)
        {
            return State != nullptr ? State->GetVelocity(Entity) : FVector3(0.0f);
        }
    }
}
