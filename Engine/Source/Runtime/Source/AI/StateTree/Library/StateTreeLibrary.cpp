#include "RuntimePCH.h"
#include "StateTreeLibraryConditions.h"
#include "StateTreeLibraryEvaluators.h"
#include "StateTreeLibraryTasks.h"

#include "AI/StateTree/StateTreeExecution.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "HierarchicalTags/HierarchicalTagComponent.h"
#include "Log/Log.h"
#include "World/World.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/StateTreeComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/AnimationSubsystem.h"

namespace Lumina
{
    namespace StateTreeLibraryInternal
    {
        // Only what CStateTreeSystem declares is read here, so a worker never touches a component another system writes beside it.
        template<typename TAccess>
        auto* ComponentOf(const FStateTreeExecutionContext& Context, ECS::FEntity Entity)
        {
            CWorld* World = Context.GetWorld();
            return World != nullptr ? World->TryGetComponent<TAccess>(Entity) : nullptr;
        }

        // The scheduler resolved dirty transforms before the batch, so the cached location is current.
        const STransformComponent* TransformOf(const FStateTreeExecutionContext& Context, ECS::FEntity Entity)
        {
            return ComponentOf<ECS::Read<STransformComponent>>(Context, Entity);
        }

        // Location stands in for a null entity, and a target entity without a transform resolves to nothing.
        bool ResolveTarget(const FStateTreeExecutionContext& Context, ECS::FEntity Entity, const FVector3& Location, FVector3& OutTarget)
        {
            if (Entity.IsNull())
            {
                OutTarget = Location;
                return true;
            }
            const STransformComponent* Transform = TransformOf(Context, Entity);
            OutTarget = Transform != nullptr ? Transform->GetWorldLocationCached() : Location;
            return Transform != nullptr;
        }

        void Aim(SPathFollowComponent& Follower, const SStateTreeMoveToTaskData& Move)
        {
            if (Move.TargetEntity.IsNull())
            {
                Follower.SetTargetLocation(Move.TargetLocation);
            }
            else
            {
                Follower.SetTargetEntity(Move.TargetEntity);
            }
        }

        CAnimationSubsystem* AnimationOf(const FStateTreeExecutionContext& Context)
        {
            return Context.GetWorld() != nullptr ? Context.GetWorld()->GetSubsystem<CAnimationSubsystem>() : nullptr;
        }
    }

    EStateTreeRunStatus SStateTreeWaitTask::EnterState(FStateTreeExecutionContext& Context) const
    {
        SStateTreeWaitTaskData& Wait = Context.GetInstanceData<SStateTreeWaitTaskData>();
        const float Duration = Context.GetRandom().RandRange(Wait.Duration - Wait.RandomDeviation, Wait.Duration + Wait.RandomDeviation);
        Wait.EndTime = Context.GetTime() + Math::Max(Duration, 0.0f);
        Context.RequestWakeAt(Wait.EndTime);
        return EStateTreeRunStatus::Running;
    }

    EStateTreeRunStatus SStateTreeWaitTask::Tick(FStateTreeExecutionContext& Context) const
    {
        return Context.GetTime() >= Context.GetInstanceData<SStateTreeWaitTaskData>().EndTime ? EStateTreeRunStatus::Succeeded : EStateTreeRunStatus::Running;
    }

    EStateTreeRunStatus SStateTreeMoveToTask::EnterState(FStateTreeExecutionContext& Context) const
    {
        SPathFollowComponent* Follower = StateTreeLibraryInternal::ComponentOf<ECS::Write<SPathFollowComponent>>(Context, Context.GetOwner());
        if (Follower == nullptr)
        {
            LOG_WARN_ONCE("State tree MoveTo on entity {} fails because it has no path follow component.", Context.GetOwner().GetPacked());
            return EStateTreeRunStatus::Failed;
        }
        StateTreeLibraryInternal::Aim(*Follower, Context.GetInstanceData<SStateTreeMoveToTaskData>());
        return EStateTreeRunStatus::Running;
    }

    EStateTreeRunStatus SStateTreeMoveToTask::Tick(FStateTreeExecutionContext& Context) const
    {
        SPathFollowComponent* Follower = StateTreeLibraryInternal::ComponentOf<ECS::Write<SPathFollowComponent>>(Context, Context.GetOwner());
        const SStateTreeMoveToTaskData& Move = Context.GetInstanceData<SStateTreeMoveToTaskData>();
        if (Follower == nullptr || (!Move.TargetEntity.IsNull() && !Context.GetWorld()->IsValidEntity(Move.TargetEntity)))
        {
            return EStateTreeRunStatus::Failed;
        }

        // A bound target lost mid move, such as a forgotten chase target, fails rather than walking to TargetLocation.
        if (Move.TargetEntity.IsNull() && !Follower->TargetEntity.IsNull())
        {
            return EStateTreeRunStatus::Failed;
        }

        // A bound target that changed, such as a new chase target, re-aims the follower.
        const bool bRetarget = Move.TargetEntity.IsNull()
            ? Math::Length(Move.TargetLocation - Follower->TargetLocation) > Follower->RepathDistance
            : Move.TargetEntity != Follower->TargetEntity;
        if (bRetarget)
        {
            StateTreeLibraryInternal::Aim(*Follower, Move);
        }

        switch (Follower->Status)
        {
            case EPathFollowStatus::Reached:
                return EStateTreeRunStatus::Succeeded;
            case EPathFollowStatus::Failed:
            case EPathFollowStatus::None:
                return EStateTreeRunStatus::Failed;
            default:
                return Follower->IsStuck() ? EStateTreeRunStatus::Failed : EStateTreeRunStatus::Running;
        }
    }

    void SStateTreeMoveToTask::ExitState(FStateTreeExecutionContext& Context) const
    {
        if (SPathFollowComponent* Follower = StateTreeLibraryInternal::ComponentOf<ECS::Write<SPathFollowComponent>>(Context, Context.GetOwner()))
        {
            Follower->Stop();
        }
    }

    EStateTreeRunStatus SStateTreeLookAtTask::Tick(FStateTreeExecutionContext& Context) const
    {
        const SStateTreeLookAtTaskData& Look = Context.GetInstanceData<SStateTreeLookAtTaskData>();
        const STransformComponent* Self = StateTreeLibraryInternal::TransformOf(Context, Context.GetOwner());
        FVector3 Target;
        if (Self == nullptr || !StateTreeLibraryInternal::ResolveTarget(Context, Look.TargetEntity, Look.TargetLocation, Target))
        {
            return EStateTreeRunStatus::Failed;
        }
        const FVector3 ToTarget = Math::Flat(Target - Self->GetWorldLocationCached());
        if (Math::Length(ToTarget) < 1e-3f)
        {
            return EStateTreeRunStatus::Succeeded;
        }

        const float Current = Math::YawDegrees(Self->GetWorldTransformCached().GetForward());
        const float Desired = Math::YawDegrees(ToTarget);
        const float Yaw = Math::MoveTowardsAngleDegrees(Current, Desired, Look.TurnRate > 0.0f ? Look.TurnRate * Context.GetDeltaTime() : 360.0f);
        if (Yaw != Current)
        {
            // Other trees may read this transform from another worker, so the turn lands at the next flush.
            Context.GetWorld()->GetCommandBus().Patch<STransformComponent>(Context.GetOwner(), [Yaw](STransformComponent& Transform)
            {
                Transform.SetRotation(Math::FromYawDegrees(Yaw));
            });
        }
        return Math::Abs(Math::DeltaAngleDegrees(Yaw, Desired)) <= Look.AcceptanceAngle ? EStateTreeRunStatus::Succeeded : EStateTreeRunStatus::Running;
    }

    EStateTreeRunStatus SStateTreePlayMontageTask::EnterState(FStateTreeExecutionContext& Context) const
    {
        CAnimationSubsystem* Animation = StateTreeLibraryInternal::AnimationOf(Context);
        // Checked here because the subsystem would add a missing graph component, a structural change a worker cannot make.
        if (Animation == nullptr || !Context.GetWorld()->HasComponent<SAnimationGraphComponent>(Context.GetOwner()))
        {
            LOG_WARN_ONCE("State tree PlayAnimMontage on entity {} fails because it has no animation graph component.", Context.GetOwner().GetPacked());
            return EStateTreeRunStatus::Failed;
        }
        const SStateTreePlayMontageTaskData& Play = Context.GetInstanceData<SStateTreePlayMontageTaskData>();
        return Animation->PlayMontage(Context.GetOwner(), Montage.Get(), Play.PlayRate, Play.Section) != 0 ? EStateTreeRunStatus::Running : EStateTreeRunStatus::Failed;
    }

    EStateTreeRunStatus SStateTreePlayMontageTask::Tick(FStateTreeExecutionContext& Context) const
    {
        CAnimationSubsystem* Animation = StateTreeLibraryInternal::AnimationOf(Context);
        return Animation != nullptr && Animation->IsMontagePlaying(Context.GetOwner(), Montage.Get()) ? EStateTreeRunStatus::Running : EStateTreeRunStatus::Succeeded;
    }

    void SStateTreePlayMontageTask::ExitState(FStateTreeExecutionContext& Context) const
    {
        CAnimationSubsystem* Animation = StateTreeLibraryInternal::AnimationOf(Context);
        if (Animation != nullptr && Montage && Animation->IsMontagePlaying(Context.GetOwner(), Montage.Get()))
        {
            Animation->StopMontage(Context.GetOwner(), Montage.Get());
        }
    }

    EStateTreeRunStatus SStateTreeInstantTaskBase::EnterState(FStateTreeExecutionContext& Context) const
    {
        if (!Execute(Context))
        {
            return EStateTreeRunStatus::Failed;
        }
        return bCompleteState ? EStateTreeRunStatus::Succeeded : EStateTreeRunStatus::Running;
    }

    bool SStateTreeSendEventTask::Execute(FStateTreeExecutionContext& Context) const
    {
        const SStateTreeSendEventTaskData& Send = Context.GetInstanceData<SStateTreeSendEventTaskData>();
        const uint32 Event = Send.Event.GetId();
        if (Event == 0)
        {
            return false;
        }
        if (Send.Target.IsNull() || Send.Target == Context.GetOwner())
        {
            return Context.SendEvent(Event);
        }
        CWorld* World = Context.GetWorld();
        if (World == nullptr || !World->HasComponent<SStateTreeComponent>(Send.Target))
        {
            return false;
        }
        // The target's tree may be ticking on another worker, so the event is queued at the next flush.
        World->GetCommandBus().Patch<SStateTreeComponent>(Send.Target, [Event](SStateTreeComponent& Tree) { Tree.SendEvent(Event); });
        return true;
    }

    bool SStateTreeSetParameterTask::Execute(FStateTreeExecutionContext& Context) const
    {
        const CStruct* ParametersType = nullptr;
        void* Parameters = Context.GetParameters(ParametersType);
        const FProperty* Target = ParametersType != nullptr ? ParametersType->GetProperty(Parameter) : nullptr;
        const TSpan<FProperty* const> Values = SStateTreeSetParameterTaskData::StaticStruct()->GetProperties();
        const auto Source = Algo::FindIf(Values, [&](const FProperty* Value) { return Target != nullptr && Value->HasSameValueType(Target); });
        if (Source == Values.end())
        {
            LOG_WARN_ONCE("State tree SetParameter has no parameter '{}' of a type it can write.", Parameter.ToString());
            return false;
        }
        Target->CopyCompleteValue(Target->GetValuePtr<void>(Parameters), (*Source)->GetValuePtr<void>(&Context.GetInstanceData<SStateTreeSetParameterTaskData>()));
        return true;
    }

    bool SStateTreeCompareCondition::TestCondition(FStateTreeExecutionContext& Context) const
    {
        const SStateTreeCompareConditionData& Values = Context.GetInstanceData<SStateTreeCompareConditionData>();
        switch (Kind)
        {
            case EStateTreeCompareKind::Bool:
                return Values.bValue;
            case EStateTreeCompareKind::Tag:
                return Values.Tag.MatchesTag(Values.MatchTag);
            case EStateTreeCompareKind::EntityValid:
                return Context.GetWorld() != nullptr && Context.GetWorld()->IsValidEntity(Values.Entity);
            case EStateTreeCompareKind::Number:
                break;
        }
        switch (Op)
        {
            case EStateTreeCompareOp::Equal:        return Values.Left == Values.Right;
            case EStateTreeCompareOp::NotEqual:     return Values.Left != Values.Right;
            case EStateTreeCompareOp::Less:         return Values.Left < Values.Right;
            case EStateTreeCompareOp::LessEqual:    return Values.Left <= Values.Right;
            case EStateTreeCompareOp::Greater:      return Values.Left > Values.Right;
            case EStateTreeCompareOp::GreaterEqual: return Values.Left >= Values.Right;
        }
        return false;
    }

    bool SStateTreeDistanceCondition::TestCondition(FStateTreeExecutionContext& Context) const
    {
        const SStateTreeDistanceConditionData& Distance = Context.GetInstanceData<SStateTreeDistanceConditionData>();
        const STransformComponent* Self = StateTreeLibraryInternal::TransformOf(Context, Context.GetOwner());
        FVector3 Target;
        return Self != nullptr && StateTreeLibraryInternal::ResolveTarget(Context, Distance.TargetEntity, Distance.TargetLocation, Target)
            && Math::Length(Target - Self->GetWorldLocationCached()) <= Distance.Radius;
    }

    bool SStateTreePerceptionSensedCondition::TestCondition(FStateTreeExecutionContext& Context) const
    {
        const SPerceptionComponent* Perception = StateTreeLibraryInternal::ComponentOf<ECS::Read<SPerceptionComponent>>(Context, Context.GetOwner());
        if (Perception == nullptr)
        {
            return false;
        }
        const ECS::FEntity Target = Context.GetInstanceData<SStateTreePerceptionSensedConditionData>().Target;
        return Algo::AnyOf(TSpan<const FPerceivedTarget>(Perception->PerceivedTargets, Perception->PerceivedCount), [&](const FPerceivedTarget& Sensed)
        {
            return (Sensed.ActiveSenses & (uint8)Senses) != 0 && (Target.IsNull() || Sensed.Target == Target);
        });
    }

    bool SStateTreeHasTagCondition::TestCondition(FStateTreeExecutionContext& Context) const
    {
        const SStateTreeHasTagConditionData& Has = Context.GetInstanceData<SStateTreeHasTagConditionData>();
        const SHierarchicalTagComponent* Tags = StateTreeLibraryInternal::ComponentOf<ECS::Read<SHierarchicalTagComponent>>(Context, Has.Target.IsNull() ? Context.GetOwner() : Has.Target);
        return Tags != nullptr && (bExact ? Tags->Tags.HasTagExact(Has.Tag) : Tags->Tags.HasTag(Has.Tag));
    }

    bool SStateTreeCooldownCondition::TestCondition(FStateTreeExecutionContext& Context) const
    {
        SStateTreeCooldownConditionData& Cooldown = Context.GetInstanceData<SStateTreeCooldownConditionData>();
        if (Context.GetTime() < Cooldown.ReadyTime)
        {
            Context.RequestWakeAt(Cooldown.ReadyTime);
            return false;
        }
        Cooldown.ReadyTime = Context.GetTime() + Cooldown.Duration;
        return true;
    }

    bool SStateTreeRandomChanceCondition::TestCondition(FStateTreeExecutionContext& Context) const
    {
        return Context.GetRandom().NextFloat() < Context.GetInstanceData<SStateTreeRandomChanceConditionData>().Chance;
    }

    void SStateTreePerceivedTargetEvaluator::Tick(FStateTreeExecutionContext& Context) const
    {
        SStateTreePerceivedTargetEvaluatorData& Out = Context.GetInstanceData<SStateTreePerceivedTargetEvaluatorData>();
        Out = SStateTreePerceivedTargetEvaluatorData();
        const SPerceptionComponent* Perception = StateTreeLibraryInternal::ComponentOf<ECS::Read<SPerceptionComponent>>(Context, Context.GetOwner());
        if (Perception == nullptr || Perception->PerceivedCount == 0)
        {
            return;
        }
        const TSpan<const FPerceivedTarget> Targets(Perception->PerceivedTargets, Perception->PerceivedCount);
        const FPerceivedTarget& Picked = Pick == EStateTreeTargetPick::Nearest
            ? Targets[Perception->Find(Perception->GetClosestPerceivedTarget())]
            : *Algo::MinElement(Targets, [](const FPerceivedTarget& A, const FPerceivedTarget& B) { return A.TimeSinceLastSensed < B.TimeSinceLastSensed; });
        Out.bHasTarget = true;
        Out.Target = Picked.Target;
        Out.Location = Picked.LastKnownLocation;
    }

    void SStateTreeSelfInfoEvaluator::Tick(FStateTreeExecutionContext& Context) const
    {
        SStateTreeSelfInfoEvaluatorData& Out = Context.GetInstanceData<SStateTreeSelfInfoEvaluatorData>();
        const STransformComponent* Transform = StateTreeLibraryInternal::TransformOf(Context, Context.GetOwner());
        if (Transform == nullptr)
        {
            return;
        }
        const FVector3 Location = Transform->GetWorldLocationCached();
        Out.Velocity = Context.GetDeltaTime() > 0.0f ? (Location - Out.Location) / Context.GetDeltaTime() : FVector3(0.0f);
        Out.Location = Location;
    }
}
