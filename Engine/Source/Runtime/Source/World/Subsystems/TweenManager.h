#pragma once

#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Easing.h"
#include "Core/Math/Math.h"
#include "Core/Templates/LuminaTemplate.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Systems/SystemResources.h"
#include "World/Subsystems/TweenTypes.h"

namespace Lumina
{
    class CCurveAsset;
    class CStruct;
    class FTweenManager;
    struct SKeyedCurve;

    namespace TweenDetail
    {
        // Math::Lerp has a generic and a vector overload that are ambiguous for FVector3, so tweens use this.
        template <typename T>
        NODISCARD constexpr T Lerp(const T& A, const T& B, float Alpha) { return A + (B - A) * Alpha; }

        // A rotation has to slerp, or a wide turn cuts through the inside of the arc.
        NODISCARD inline FQuat Lerp(const FQuat& A, const FQuat& B, float Alpha) { return Math::Slerp(A, B, Alpha); }
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

        //~ Entity transform conveniences, the common case. Each samples its start when the step begins.

        FTween& MoveTo(ECS::FEntity Entity, const FVector3& Target, float Duration);
        FTween& RotateTo(ECS::FEntity Entity, const FQuat& Target, float Duration);
        FTween& ScaleTo(ECS::FEntity Entity, const FVector3& Target, float Duration);

        // Relative to wherever the entity is when the step begins.
        FTween& MoveBy(ECS::FEntity Entity, const FVector3& Offset, float Duration);
        FTween& RotateBy(ECS::FEntity Entity, const FQuat& Delta, float Duration);

        // Multiplies the starting scale, so a factor of two doubles it.
        FTween& ScaleBy(ECS::FEntity Entity, const FVector3& Factor, float Duration);

        // A kick toward Punch that swings back and settles, rotation in degrees. Vibrato counts the swings.
        FTween& Punch(ECS::FEntity Entity, ETweenTransformChannel Channel, const FVector3& Punch, float Duration, int32 Vibrato = 10, float Elasticity = 1.0f);

        // A decaying random wobble of up to Strength on each axis, rotation in degrees. Vibrato counts the swings.
        FTween& Shake(ECS::FEntity Entity, ETweenTransformChannel Channel, const FVector3& Strength, float Duration, int32 Vibrato = 10);

        //~ Reflected component properties, found by a dotted path such as "Settings.Intensity".

        FTween& PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, float Target, float Duration);
        FTween& PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FVector2& Target, float Duration);
        FTween& PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FVector3& Target, float Duration);
        FTween& PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FVector4& Target, float Duration);
        FTween& PropertyTo(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const FQuat& Target, float Duration);

        template <typename TComponent, typename T>
        FTween& PropertyTo(ECS::FEntity Entity, FStringView Path, const T& Target, float Duration)
        {
            return PropertyTo(Entity, TComponent::StaticStruct(), Path, Target, Duration);
        }

        //~ Modifiers. Transition, ease, curve and delay apply to the most recently added tweener.

        FTween& Trans(EEaseTransition Transition);
        FTween& Ease(EEaseType Ease);

        // Shapes the step with a curve over its keyed time range, replacing Trans and Ease.
        FTween& EaseCurve(const CCurveAsset* Curve);
        FTween& EaseCurve(const SKeyedCurve& Curve);

        FTween& Delay(float Seconds);

        /** Puts the next tweener in the same step as the last, so they run together. */
        FTween& Parallel();

        /** 0 repeats forever, 1 is the default single pass. */
        FTween& SetLoops(int32 Count);
        FTween& SetLoops(int32 Count, ETweenLoopMode Mode);
        FTween& SetLoopMode(ETweenLoopMode Mode);
        FTween& SetSpeedScale(float Scale);

        // Runs on real time, through world pause and time dilation, for menus and other UI.
        FTween& SetIgnoreTimeScale(bool bIgnore);

        FTween& OnFinished(TFunction<void()> Callback);

        // Fires as each pass ends and another begins, with the number of passes completed so far.
        FTween& OnLoop(TFunction<void(int32)> Callback);

        // Fires as each step ends, with that step's index in the order it was built.
        FTween& OnStep(TFunction<void(int32)> Callback);

        /** Stops where it is; whatever it was driving keeps its current value. */
        void Kill();

        // Jumps to the end of the current pass, running every step and OnFinished on the way.
        void Complete();

        void SetPaused(bool bPaused);

        NODISCARD bool IsRunning() const;

        // Seconds into the current pass, the length of one pass, and the first over the second.
        NODISCARD float GetElapsed() const;
        NODISCARD float GetDuration() const;
        NODISCARD float GetProgress() const;
        NODISCARD int32 GetCompletedLoops() const;

        NODISCARD FTweenHandle GetHandle() const { return Handle; }

    private:

        FTween& AddValueTweener(float Duration, TFunction<void(float)> Apply);
        FTween& AddTransformTweener(ECS::FEntity Entity, ETweenTransformChannel Channel, float Duration, TFunction<FVector3(const FVector3&, float)> LocationOrScale, TFunction<FQuat(const FQuat&, float)> Rotation);
        FTween& AddOffsetTweener(ECS::FEntity Entity, ETweenTransformChannel Channel, float Duration, TFunction<FVector3(float)> OffsetAt);

        template <typename T>
        FTween& AddPropertyTweener(ECS::FEntity Entity, const CStruct* Component, FStringView Path, const T& Target, float Duration);

        FTweenManager* Manager = nullptr;
        FTweenHandle   Handle;
    };

    /** Per-world tween runner, advanced once per frame by STweenSystem. */
    class RUNTIME_API FTweenManager : public ECS::FResource
    {
    public:

        FTweenManager() = default;
        ~FTweenManager();
        LE_NO_COPYMOVE(FTweenManager);

        NODISCARD FTween Create();

        /** Killed automatically when Owner dies, which is what a gameplay tween almost always wants. */
        NODISCARD FTween CreateForEntity(ECS::FEntity Owner);

        void Kill(FTweenHandle Handle);
        void Complete(FTweenHandle Handle);
        void KillTweensForEntity(ECS::FEntity Owner);
        void Clear();

        NODISCARD bool IsRunning(FTweenHandle Handle) const;

        NODISCARD bool HasTweens() const { return Registry.NumEntities() != 0; }

        /** The registry the transform helpers write through, and the one owner liveness is checked against. */
        void SetWorldRegistry(ECS::FRegistry* InRegistry) { WorldRegistry = InRegistry; }

        // Scaled time for ordinary tweens and real time for those that ignore time scale.
        void Tick(float DeltaTime, float UnscaledDeltaTime);
        void Tick(float DeltaTime) { Tick(DeltaTime, DeltaTime); }

    private:

        friend class FTween;

        enum class ETweenerKind : uint8 { Value, Interval, Callback };

        struct FTweener
        {
            ETweenerKind               Kind       = ETweenerKind::Value;
            float                      Duration   = 0.0f;
            float                      Delay      = 0.0f;
            float                      Elapsed    = 0.0f;
            EEaseTransition            Transition = EEaseTransition::Linear;
            EEaseType                  EaseType   = EEaseType::InOut;
            bool                       bFinished  = false;
            TFunction<void(float)>     Apply;
            TFunction<void()>          Callback;
            TFunction<float(float)>    CustomEase;
        };

        struct FStep
        {
            TVector<FTweener> Tweeners;
        };

        struct FTweenState
        {
            TVector<FStep>         Steps;
            int32                  StepIndex        = 0;
            int32                  LoopsRemaining   = 1;
            int32                  CompletedLoops   = 0;
            float                  SpeedScale       = 1.0f;
            ETweenLoopMode         LoopMode         = ETweenLoopMode::Restart;
            bool                   bPaused          = false;
            bool                   bPendingKill     = false;
            bool                   bPendingComplete = false;
            bool                   bParallelPending = false;
            bool                   bIgnoreTimeScale = false;
            bool                   bReversed        = false;
            ECS::FEntity           Owner            = ECS::NullEntity;
            TFunction<void()>      FinishedCallback;
            TFunction<void(int32)> LoopCallback;
            TFunction<void(int32)> StepCallback;
        };

        FTweenState* Find(FTweenHandle Handle);
        const FTweenState* Find(FTweenHandle Handle) const;
        FTweener*    LastTweener(FTweenHandle Handle);
        void         PushTweener(FTweenHandle Handle, FTweener&& Tweener);

        /** Advances one tween, returning true once every pass has run. */
        bool AdvanceTween(FTweenState& State, float DeltaTime);

        // Runs the current pass's remaining steps, returning the time left over once the pass ends.
        float AdvanceSteps(FTweenState& State, float Remaining);

        void RestartPass(FTweenState& State);

        // Finishes the current pass at once, returning true so the caller retires the tween.
        bool FinishNow(FTweenState& State);

        static float StepLength(const FStep& Step);

        mutable ECS::FRegistry Registry;
        ECS::FRegistry*        WorldRegistry = nullptr;
        uint32                 NextShakeSeed = 1;
        bool                   bTicking = false;
    };
}
