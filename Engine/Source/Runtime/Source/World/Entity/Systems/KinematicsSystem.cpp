#include "RuntimePCH.h"
#include "KinematicsSystem.h"
#include "World/ECS/Registry.h"

#include "Core/Console/ConsoleVariable.h"
#include "Physics/PhysicsScene.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
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

    void SKinematicsSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Highest);
        RequireUpdate(EUpdateStage::Paused, EUpdatePriority::Highest);
        Writes<SVelocityComponent, SystemResource::Kinematics>();
        Reads<STransformComponent, SCharacterMovementComponent, SRigidBodyComponent, SystemResource::PhysicsQuery>();
        Reads<SSkeletalMeshComponent, SAudioSourceComponent, SAudioListenerComponent, SCameraComponent>();
    }

    namespace
    {
        constexpr uint32 kKinematicsParallelGrain = 2048;
        constexpr uint32 kKinematicsInvalidBody = Constants::kIndexNoneU32;
    }

    void SKinematicsSystem::OnStartup()
    {
        const FSystemContext& Context = GetContext();

        Context.GetRegistry().Ctx().Emplace<FKinematicsState>();
    }

    void SKinematicsSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

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

        // Only the components whose systems read a velocity, since differencing every transform paid for static scenery and crowds.
        const ECS::FSparseSet* Consumers[] =
        {
            Context.CreateView<SSkeletalMeshComponent>().GetDriver(),
            Context.CreateView<SAudioSourceComponent>().GetDriver(),
            Context.CreateView<SAudioListenerComponent>().GetDriver(),
            Context.CreateView<SCameraComponent>().GetDriver(),
            Context.CreateView<SVelocityComponent>().GetDriver(),
        };

        uint32 MaxIndex = 0;
        bool bAnyConsumer = false;
        for (const ECS::FSparseSet* Pool : Consumers)
        {
            if (Pool == nullptr || Pool->IsEmpty())
            {
                continue;
            }
            bAnyConsumer = true;
            MaxIndex = Math::Max(MaxIndex, Pool->MaxLiveIndex());
        }

        ++State.Stamp;
        if (!bAnyConsumer)
        {
            return;
        }

        if ((uint32)State.ByEntityIndex.size() <= MaxIndex)
        {
            State.ByEntityIndex.resize((size_t)MaxIndex + 1u);
        }

        auto TransformStorage = Context.GetStorage<STransformComponent>();

        const uint32 Stamp     = State.Stamp;
        const float  DeltaTime = (float)Context.GetDeltaTime();
        const float  InvDelta  = DeltaTime > 0.0f ? (1.0f / DeltaTime) : 0.0f;
        FEntityKinematics* Entries = State.ByEntityIndex.data();

        // An entity in two consumer pools is differenced once, since the second visit sees this pass's stamp.
        const auto Difference = [&](ECS::FEntity Entity)
        {
            const STransformComponent* Xform = Entity.IsTombstone() ? nullptr : TransformStorage.TryGet(Entity);
            if (Xform == nullptr)
            {
                return;
            }

            FEntityKinematics& Entry = Entries[Entity.GetIndex()];
            if (Entry.Owner == Entity && Entry.Stamp == Stamp)
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
        for (const ECS::FSparseSet* Pool : Consumers)
        {
            if (Pool == nullptr || Pool->IsEmpty())
            {
                continue;
            }

            const ECS::FEntity* Dense = Pool->GetDenseData();
            const uint32 Num = (uint32)Pool->GetDenseSize();
            if (Num < kKinematicsParallelGrain || GTaskSystem == nullptr)
            {
                for (uint32 i = 0; i < Num; ++i)
                {
                    Difference(Dense[i]);
                }
                continue;
            }

            Task::ParallelFor(Num, [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    Difference(Dense[i]);
                }
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
            const auto RefineBody = [&](ECS::FEntity Entity, const SRigidBodyComponent&, const STransformComponent&)
            {
                // Most bodies have no consumer, and the stamp check is far cheaper than the scene's status lookup.
                const uint32 Index = Entity.GetIndex();
                if (Index >= (uint32)State.ByEntityIndex.size() || Entries[Index].Owner != Entity || Entries[Index].Stamp != Stamp)
                {
                    return;
                }
                if (Scene->GetBodyStatus(Entity) == Physics::EPhysicsBodyStatus::Ready)
                {
                    Refine(Entity, Scene->GetLinearVelocity(Entity));
                }
            };

            // The world is not stepping before physics, so its body reads are safe from every worker at once.
            auto BodyView = Context.CreateView<SRigidBodyComponent, STransformComponent>();
            if (BodyView.NumDenseSlots() < kKinematicsParallelGrain || GTaskSystem == nullptr)
            {
                BodyView.ForEach(RefineBody);
            }
            else
            {
                Task::ParallelFor((uint32)BodyView.NumDenseSlots(), [&](const Task::FParallelRange& Range)
                {
                    BodyView.ForEachInRange(Range.Start, Range.End, RefineBody);
                }, 256);
            }
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
