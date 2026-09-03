#pragma once

#include "Containers/Function.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Easing.h"
#include "Core/Math/Math.h"
#include "Core/Templates/LuminaTemplate.h"
#include "World/ECS/Registry.h"

namespace Lumina
{
    class FTweenManager;

    namespace TweenDetail
    {
        // Math::Lerp has a generic and a vector overload that are ambiguous for FVector3, so tweens use this.
        template <typename T>
        NODISCARD constexpr T Lerp(const T& A, const T& B, float Alpha) { return A + (B - A) * Alpha; }
    }

    /** Survives the tween it names; a stale one reports false from IsRunning rather than dangling. */
    struct FTweenHandle
    {
        ECS::FEntity Handle = ECS::NullEntity;

        bool IsValid() const { return Handle != ECS::NullEntity; }
        void Invalidate()    { Handle = ECS::NullEntity; }

        bool operator==(const FTweenHandle& Other) const { return Handle == Other.Handle; }
    };

    /** Fluent builder over one tween. Copyable, and safe to keep after the tween finishes. */
    class RUNTIME_API FTween
    {
    public:

        FTween() = default;
        FTween(FTweenManager* InManager, FTweenHandle InHandle) : Manager(InManager), Handle(InHandle) {}

        //~ Tweeners. Each starts a new sequential step unless Parallel() preceded it.

        /** Interpolates From to Target over Duration, handing each value to Setter. */
        template <typename T, typename TSetter>
        FTween& To(T From, T Target, float Duration, TSetter&& Setter)
        {
            // Setter is its own parameter, or deducing T from a TFunction parameter rejects every lambda.
            return AddValueTweener(Duration, [From, Target, Setter = Forward<TSetter>(Setter)](float Alpha)
            {
                Setter(TweenDetail::Lerp(From, Target, Alpha));
            });
        }

        /** Dead time, for spacing steps apart. */
        FTween& Interval(float Duration);

        /** Fires once when the step is reached. */
        FTween& Call(TFunction<void()> Callback);

        //~ Entity transform conveniences, the common case.

        FTween& MoveTo(ECS::FEntity Entity, const FVector3& Target, float Duration);
        FTween& RotateTo(ECS::FEntity Entity, const FQuat& Target, float Duration);
        FTween& ScaleTo(ECS::FEntity Entity, const FVector3& Target, float Duration);

        //~ Modifiers. Transition, ease and delay apply to the most recently added tweener.

        FTween& Trans(EEaseTransition Transition);
        FTween& Ease(EEaseType Ease);
        FTween& Delay(float Seconds);

        /** Puts the next tweener in the same step as the last, so they run together. */
        FTween& Parallel();

        /** 0 repeats forever, 1 is the default single pass. */
        FTween& SetLoops(int32 Count);
        FTween& SetSpeedScale(float Scale);

        /** Runs the whole tween unscaled, so a paused or slowed world still animates it. */
        FTween& SetIgnoreTimeScale(bool bIgnore);

        FTween& OnFinished(TFunction<void()> Callback);

        void Kill();
        void SetPaused(bool bPaused);

        NODISCARD bool IsRunning() const;
        NODISCARD FTweenHandle GetHandle() const { return Handle; }

    private:

        FTween& AddValueTweener(float Duration, TFunction<void(float)> Apply);

        FTweenManager* Manager = nullptr;
        FTweenHandle   Handle;
    };

    /** Per-world tween runner, advanced once per frame from the world's tick. */
    class RUNTIME_API FTweenManager
    {
    public:

        FTweenManager() = default;
        ~FTweenManager();
        LE_NO_COPYMOVE(FTweenManager);

        NODISCARD FTween Create();

        /** Killed automatically when Owner dies, which is what a gameplay tween almost always wants. */
        NODISCARD FTween CreateForEntity(ECS::FEntity Owner);

        void Kill(FTweenHandle Handle);
        void KillTweensForEntity(ECS::FEntity Owner);
        void Clear();

        NODISCARD bool IsRunning(FTweenHandle Handle) const;

        /** The registry the transform helpers write through, and the one owner liveness is checked against. */
        void SetWorldRegistry(ECS::FRegistry* InRegistry) { WorldRegistry = InRegistry; }

        void Tick(float DeltaTime, float UnscaledDeltaTime);

    private:

        friend class FTween;

        enum class ETweenerKind : uint8 { Value, Interval, Callback };

        struct FTweener
        {
            ETweenerKind            Kind       = ETweenerKind::Value;
            float                   Duration   = 0.0f;
            float                   Delay      = 0.0f;
            float                   Elapsed    = 0.0f;
            EEaseTransition         Transition = EEaseTransition::Linear;
            EEaseType               EaseType   = EEaseType::InOut;
            bool                    bFinished  = false;
            TFunction<void(float)>  Apply;
            TFunction<void()>       Callback;
        };

        struct FStep
        {
            TVector<FTweener> Tweeners;
        };

        struct FTweenState
        {
            TVector<FStep>    Steps;
            int32             StepIndex        = 0;
            int32             LoopsRemaining   = 1;
            float             SpeedScale       = 1.0f;
            bool              bPaused          = false;
            bool              bIgnoreTimeScale = false;
            bool              bPendingKill     = false;
            bool              bParallelPending = false;
            ECS::FEntity      Owner            = ECS::NullEntity;
            TFunction<void()> FinishedCallback;
        };

        FTweenState* Find(FTweenHandle Handle);
        FTweener*    LastTweener(FTweenHandle Handle);
        void         PushTweener(FTweenHandle Handle, FTweener&& Tweener);

        /** Advances one tween, returning true once every step has run. */
        bool AdvanceTween(FTweenState& State, float DeltaTime);

        mutable ECS::FRegistry Registry;
        ECS::FRegistry*        WorldRegistry = nullptr;
        bool                   bTicking = false;
    };
}
