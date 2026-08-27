#include "RuntimePCH.h"
#include "KinematicsSystem.h"
#include "World/ECS/Registry.h"

#include "Core/Console/ConsoleVariable.h"
#include "Physics/PhysicsScene.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/VelocityComponent.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarKinematicsEnabled("Kinematics.Enabled", true,
        "Resolve a shared per-entity velocity each frame. Off makes every lookup report zero.");

    FSystemAccess SKinematicsSystem::Access = FSystemAccess{}
        .Write<SVelocityComponent, SystemResource::Kinematics>()
        .Read<STransformComponent, SCharacterMovementComponent, SRigidBodyComponent, SystemResource::PhysicsQuery>();

    namespace
    {
        constexpr uint32 kKinematicsParallelGrain = 2048;
        constexpr uint32 kKinematicsInvalidBody = 0xFFFFFFFFu;
    }

    void SKinematicsSystem::Startup(const FSystemContext& Context) noexcept
    {
        Context.GetRegistry().Ctx().Emplace<FKinematicsState>();
    }

    void SKinematicsSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        FKinematicsState* StatePtr = Context.GetRegistry().Ctx().Find<FKinematicsState>();
        if (StatePtr == nullptr)
        {
            return;
        }

        FKinematicsState& State = *StatePtr;
        State.bEnabled = CVarKinematicsEnabled.GetValue();
        if (!State.bEnabled)
        {
            return;
        }

        auto MotionView = Context.CreateView<STransformComponent>();
        const ECS::FSparseSet* Driver = MotionView.GetDriver();
        if (Driver == nullptr || Driver->IsEmpty())
        {
            return;
        }

        const ECS::FEntity* Dense = Driver->GetDenseData();
        const size_t   DenseNum   = Driver->GetDenseSize();
        uint32 MaxIndex = 0;
        for (size_t i = 0; i < DenseNum; ++i)
        {
            MaxIndex = Math::Max(MaxIndex, Dense[i].GetIndex());
        }

        if ((uint32)State.ByEntityIndex.size() <= MaxIndex)
        {
            State.ByEntityIndex.resize((size_t)MaxIndex + 1u);
        }

        ++State.Stamp;

        const uint32 Stamp     = State.Stamp;
        const float  DeltaTime = (float)Context.GetDeltaTime();
        const float  InvDelta  = DeltaTime > 0.0f ? (1.0f / DeltaTime) : 0.0f;
        FEntityKinematics* Entries = State.ByEntityIndex.data();

        // Probe-free, so the pass that has to touch every entity never leaves the transform storage.
        const auto Difference = [&](ECS::FEntity Entity)
        {
            const FVector3 Location = MotionView.Get<STransformComponent>(Entity).GetWorldLocationCached();

            FEntityKinematics& Entry = Entries[Entity.GetIndex()];
            if (Entry.Owner != Entity)
            {
                Entry.bHasPrevious = false;
            }

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

        if (DenseNum < kKinematicsParallelGrain || GTaskSystem == nullptr)
        {
            for (ECS::FEntity Entity : MotionView)
            {
                Difference(Entity);
            }
        }
        else
        {
            Task::ParallelFor((uint32)MotionView.NumDenseSlots(), [&](const Task::FParallelRange& Range)
            {
                MotionView.ForEachInRange(Range.Start, Range.End, [&](ECS::FEntity Entity, STransformComponent&)
                {
                    Difference(Entity);
                });
            }, 256);
        }

        // Refines only what the difference pass stamped, so a body with no transform is skipped.
        const auto Refine = [&](ECS::FEntity Entity, const FVector3& Velocity)
        {
            FEntityKinematics& Entry = Entries[Entity.GetIndex()];
            if (Entry.Owner != Entity || Entry.Stamp != Stamp)
            {
                return;
            }
            Entry.LinearVelocity = Velocity;
            Entry.Speed          = Math::Length(Velocity);
        };

        // The solver's own value beats a difference, which smears a collision across the whole frame.
        if (Physics::IPhysicsScene* Scene = Context.GetPhysicsScene())
        {
            Context.CreateView<SRigidBodyComponent, STransformComponent>().ForEach(
                [&](ECS::FEntity Entity, const SRigidBodyComponent& Body, const STransformComponent&)
                {
                    if (Body.BodyID != kKinematicsInvalidBody)
                    {
                        Refine(Entity, Scene->GetLinearVelocity(Body.BodyID));
                    }
                });
        }

        // Last, because a character mover owns its velocity outright and SAnimationSystem preferred it.
        Context.CreateView<SCharacterMovementComponent, STransformComponent>().ForEach(
            [&](ECS::FEntity Entity, const SCharacterMovementComponent& Movement, const STransformComponent&)
            {
                Refine(Entity, Movement.Velocity);
            });


        // Mirrored so the reflected component reads the same value in the editor and in script.
        Context.CreateView<SVelocityComponent>().ForEach(
            [&](ECS::FEntity Entity, SVelocityComponent& Out)
            {
                const uint32 Index = Entity.GetIndex();
                const bool bFresh = Index < (uint32)State.ByEntityIndex.size()
                                 && Entries[Index].Owner == Entity
                                 && Entries[Index].Stamp == Stamp;
                Out.Velocity = bFresh ? Entries[Index].LinearVelocity : FVector3(0.0f);
                Out.Speed    = bFresh ? Entries[Index].Speed : 0.0f;
            });
    }

    namespace Kinematics
    {
        const FKinematicsState* GetState(const FSystemContext& Context)
        {
            return Context.GetRegistry().Ctx().Find<FKinematicsState>();
        }

        const FKinematicsState* GetState(CWorld* World)
        {
            if (World == nullptr)
            {
                return nullptr;
            }
            return ECS::GetWorldRegistry(*World).Ctx().Find<FKinematicsState>();
        }
    }
}
