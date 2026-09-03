#include "RuntimePCH.h"
#include "TweenManager.h"

#include "Containers/Optional.h"
#include "Memory/SmartPtr.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace
    {
        // A zero-length tweener still has to run its final value once, so it lands on the target.
        constexpr float kMinDuration = 1e-6f;
    }

    FTweenManager::~FTweenManager()
    {
        Clear();
    }

    FTween FTweenManager::Create()
    {
        return CreateForEntity(ECS::NullEntity);
    }

    FTween FTweenManager::CreateForEntity(ECS::FEntity Owner)
    {
        const ECS::FEntity Entity = Registry.Create();

        FTweenState& State = Registry.Emplace<FTweenState>(Entity);
        State.Owner = Owner;

        FTweenHandle Handle;
        Handle.Handle = Entity;
        return FTween(this, Handle);
    }

    FTweenManager::FTweenState* FTweenManager::Find(FTweenHandle Handle)
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return nullptr;
        }
        return Registry.TryGet<FTweenState>(Handle.Handle);
    }

    void FTweenManager::Kill(FTweenHandle Handle)
    {
        FTweenState* State = Find(Handle);
        if (State == nullptr)
        {
            return;
        }

        if (bTicking)
        {
            State->bPendingKill = true;
            return;
        }
        Registry.Destroy(Handle.Handle);
    }

    void FTweenManager::KillTweensForEntity(ECS::FEntity Owner)
    {
        if (Owner == ECS::NullEntity)
        {
            return;
        }

        TVector<ECS::FEntity> Doomed;
        auto View = Registry.View<FTweenState>();
        for (ECS::FEntity Entity : View)
        {
            if (View.Get<FTweenState>(Entity).Owner == Owner)
            {
                Doomed.push_back(Entity);
            }
        }

        for (ECS::FEntity Entity : Doomed)
        {
            if (bTicking)
            {
                Registry.Get<FTweenState>(Entity).bPendingKill = true;
            }
            else
            {
                Registry.Destroy(Entity);
            }
        }
    }

    void FTweenManager::Clear()
    {
        Registry.Clear();
    }

    bool FTweenManager::IsRunning(FTweenHandle Handle) const
    {
        if (!Handle.IsValid() || !Registry.IsValid(Handle.Handle))
        {
            return false;
        }

        const FTweenState* State = Registry.TryGet<FTweenState>(Handle.Handle);
        return State != nullptr && !State->bPendingKill;
    }

    void FTweenManager::PushTweener(FTweenHandle Handle, FTweener&& Tweener)
    {
        FTweenState* State = Find(Handle);
        if (State == nullptr)
        {
            return;
        }

        if (State->bParallelPending && !State->Steps.empty())
        {
            State->Steps.back().Tweeners.push_back(Move(Tweener));
        }
        else
        {
            FStep& Step = State->Steps.emplace_back();
            Step.Tweeners.push_back(Move(Tweener));
        }
        State->bParallelPending = false;
    }

    FTweenManager::FTweener* FTweenManager::LastTweener(FTweenHandle Handle)
    {
        FTweenState* State = Find(Handle);
        if (State == nullptr || State->Steps.empty() || State->Steps.back().Tweeners.empty())
        {
            return nullptr;
        }
        return &State->Steps.back().Tweeners.back();
    }

    bool FTweenManager::AdvanceTween(FTweenState& State, float DeltaTime)
    {
        if (State.bPaused)
        {
            return false;
        }

        float Remaining = DeltaTime * Math::Max(State.SpeedScale, 0.0f);

        // Looped so a frame long enough to finish a step still starts the next one in the same tick.
        while (Remaining > 0.0f && State.StepIndex < (int32)State.Steps.size())
        {
            FStep& Step = State.Steps[State.StepIndex];

            bool  bStepFinished = true;
            float StepConsumed  = 0.0f;

            for (FTweener& Tweener : Step.Tweeners)
            {
                if (Tweener.bFinished)
                {
                    continue;
                }

                const float Total = Tweener.Delay + Math::Max(Tweener.Duration, 0.0f);
                const float Before = Tweener.Elapsed;
                Tweener.Elapsed = Math::Min(Tweener.Elapsed + Remaining, Total);
                StepConsumed = Math::Max(StepConsumed, Tweener.Elapsed - Before);

                const float Active = Tweener.Elapsed - Tweener.Delay;
                if (Active < 0.0f)
                {
                    bStepFinished = false;
                    continue;
                }

                switch (Tweener.Kind)
                {
                case ETweenerKind::Value:
                {
                    const float Raw = (Tweener.Duration <= kMinDuration)
                                    ? 1.0f
                                    : Math::Clamp(Active / Tweener.Duration, 0.0f, 1.0f);

                    if (Tweener.Apply)
                    {
                        Tweener.Apply(Easing::Evaluate(Tweener.Transition, Tweener.EaseType, Raw));
                    }
                    break;
                }

                case ETweenerKind::Callback:
                    if (Tweener.Callback)
                    {
                        Tweener.Callback();
                    }
                    break;

                case ETweenerKind::Interval:
                    break;
                }

                if (Tweener.Elapsed >= Total)
                {
                    Tweener.bFinished = true;
                }
                else
                {
                    bStepFinished = false;
                }
            }

            if (!bStepFinished)
            {
                break;
            }

            // Whatever the longest tweener did not use rolls into the next step.
            Remaining -= Math::Max(StepConsumed, kMinDuration);
            ++State.StepIndex;
        }

        if (State.StepIndex < (int32)State.Steps.size())
        {
            return false;
        }

        if (State.LoopsRemaining == 0 || --State.LoopsRemaining > 0)
        {
            State.StepIndex = 0;
            for (FStep& Step : State.Steps)
            {
                for (FTweener& Tweener : Step.Tweeners)
                {
                    Tweener.Elapsed   = 0.0f;
                    Tweener.bFinished = false;
                }
            }
            return false;
        }

        return true;
    }

    void FTweenManager::Tick(float DeltaTime)
    {
        bTicking = true;

        TVector<ECS::FEntity> Completed;

        auto View = Registry.View<FTweenState>();
        for (ECS::FEntity Entity : View)
        {
            FTweenState& State = View.Get<FTweenState>(Entity);

            // An owner that has gone away takes its tweens with it, so a setter cannot touch a dead entity.
            const bool bOwnerGone = State.Owner != ECS::NullEntity
                                 && WorldRegistry != nullptr
                                 && !WorldRegistry->IsValid(State.Owner);

            if (State.bPendingKill || bOwnerGone)
            {
                Completed.push_back(Entity);
                continue;
            }

            if (AdvanceTween(State, DeltaTime))
            {
                if (State.FinishedCallback)
                {
                    State.FinishedCallback();
                }
                Completed.push_back(Entity);
            }
        }

        bTicking = false;

        for (ECS::FEntity Entity : Completed)
        {
            if (Registry.IsValid(Entity))
            {
                Registry.Destroy(Entity);
            }
        }
    }

    //~ FTween

    FTween& FTween::AddValueTweener(float Duration, TFunction<void(float)> Apply)
    {
        if (Manager != nullptr)
        {
            FTweenManager::FTweener Tweener;
            Tweener.Kind     = FTweenManager::ETweenerKind::Value;
            Tweener.Duration = Duration;
            Tweener.Apply    = Move(Apply);
            Manager->PushTweener(Handle, Move(Tweener));
        }
        return *this;
    }

    FTween& FTween::Interval(float Duration)
    {
        if (Manager != nullptr)
        {
            FTweenManager::FTweener Tweener;
            Tweener.Kind     = FTweenManager::ETweenerKind::Interval;
            Tweener.Duration = Duration;
            Manager->PushTweener(Handle, Move(Tweener));
        }
        return *this;
    }

    FTween& FTween::Call(TFunction<void()> Callback)
    {
        if (Manager != nullptr)
        {
            FTweenManager::FTweener Tweener;
            Tweener.Kind     = FTweenManager::ETweenerKind::Callback;
            Tweener.Duration = 0.0f;
            Tweener.Callback = Move(Callback);
            Manager->PushTweener(Handle, Move(Tweener));
        }
        return *this;
    }

    FTween& FTween::MoveTo(ECS::FEntity Entity, const FVector3& Target, float Duration)
    {
        if (Manager == nullptr || Manager->WorldRegistry == nullptr)
        {
            return *this;
        }

        ECS::FRegistry* Reg = Manager->WorldRegistry;

        // Start is sampled on the first apply, so a chained move begins wherever the previous step ended.
        auto Start = MakeShared<TOptional<FVector3>>();
        return AddValueTweener(Duration, [Reg, Entity, Target, Start](float Alpha)
        {
            if (!Reg->IsValid(Entity) || !Reg->HasAll<STransformComponent>(Entity))
            {
                return;
            }

            STransformComponent& Transform = Reg->Get<STransformComponent>(Entity);
            if (!Start->IsSet())
            {
                *Start = Transform.GetLocalLocation();
            }
            Transform.SetLocalLocation(TweenDetail::Lerp(Start->GetValue(), Target, Alpha));
        });
    }

    FTween& FTween::RotateTo(ECS::FEntity Entity, const FQuat& Target, float Duration)
    {
        if (Manager == nullptr || Manager->WorldRegistry == nullptr)
        {
            return *this;
        }

        ECS::FRegistry* Reg = Manager->WorldRegistry;

        auto Start = MakeShared<TOptional<FQuat>>();
        return AddValueTweener(Duration, [Reg, Entity, Target, Start](float Alpha)
        {
            if (!Reg->IsValid(Entity) || !Reg->HasAll<STransformComponent>(Entity))
            {
                return;
            }

            STransformComponent& Transform = Reg->Get<STransformComponent>(Entity);
            if (!Start->IsSet())
            {
                *Start = Transform.GetLocalRotation();
            }
            // Slerp rather than Lerp, or a wide turn cuts through the inside of the arc.
            Transform.SetLocalRotation(Math::Slerp(Start->GetValue(), Target, Alpha));
        });
    }

    FTween& FTween::ScaleTo(ECS::FEntity Entity, const FVector3& Target, float Duration)
    {
        if (Manager == nullptr || Manager->WorldRegistry == nullptr)
        {
            return *this;
        }

        ECS::FRegistry* Reg = Manager->WorldRegistry;

        auto Start = MakeShared<TOptional<FVector3>>();
        return AddValueTweener(Duration, [Reg, Entity, Target, Start](float Alpha)
        {
            if (!Reg->IsValid(Entity) || !Reg->HasAll<STransformComponent>(Entity))
            {
                return;
            }

            STransformComponent& Transform = Reg->Get<STransformComponent>(Entity);
            if (!Start->IsSet())
            {
                *Start = Transform.GetLocalScale();
            }
            Transform.SetLocalScale(TweenDetail::Lerp(Start->GetValue(), Target, Alpha));
        });
    }

    FTween& FTween::Trans(EEaseTransition Transition)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
            {
                Tweener->Transition = Transition;
            }
        }
        return *this;
    }

    FTween& FTween::Ease(EEaseType InEase)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
            {
                Tweener->EaseType = InEase;
            }
        }
        return *this;
    }

    FTween& FTween::Delay(float Seconds)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweener* Tweener = Manager->LastTweener(Handle))
            {
                Tweener->Delay = Math::Max(Seconds, 0.0f);
            }
        }
        return *this;
    }

    FTween& FTween::Parallel()
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->bParallelPending = true;
            }
        }
        return *this;
    }

    FTween& FTween::SetLoops(int32 Count)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->LoopsRemaining = Math::Max(Count, 0);
            }
        }
        return *this;
    }

    FTween& FTween::SetSpeedScale(float Scale)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->SpeedScale = Math::Max(Scale, 0.0f);
            }
        }
        return *this;
    }

    FTween& FTween::OnFinished(TFunction<void()> Callback)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->FinishedCallback = Move(Callback);
            }
        }
        return *this;
    }

    void FTween::Kill()
    {
        if (Manager != nullptr)
        {
            Manager->Kill(Handle);
        }
    }

    void FTween::SetPaused(bool bPaused)
    {
        if (Manager != nullptr)
        {
            if (FTweenManager::FTweenState* State = Manager->Find(Handle))
            {
                State->bPaused = bPaused;
            }
        }
    }

    bool FTween::IsRunning() const
    {
        return Manager != nullptr && Manager->IsRunning(Handle);
    }
}
