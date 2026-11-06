#pragma once

#include "Core/Math/Easing.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "Scripting/ScriptCallback.h"
#include "Containers/String.h"
#include "World/ECS/Entity.h"
#include "World/Subsystems/TweenManager.h"
#include "World/Subsystems/TweenTypes.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "TweenSubsystem.generated.h"

namespace Lumina
{
    class CCurveAsset;

    // Script tweens, built step by step on a tween id from CreateId.
    REFLECT()
    class RUNTIME_API CTweenSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        /** Starts an empty tween and returns its id. Owner ties its lifetime to that entity. */
        FUNCTION()
        uint32 CreateId(ECS::FEntity Owner);

        FUNCTION()
        void MoveTo(uint32 Tween, ECS::FEntity Target, FVector3 Destination, float Duration);

        FUNCTION()
        void RotateTo(uint32 Tween, ECS::FEntity Target, FQuat Destination, float Duration);

        FUNCTION()
        void ScaleTo(uint32 Tween, ECS::FEntity Target, FVector3 Destination, float Duration);

        // Relative to wherever Target is when the step begins. ScaleBy multiplies, so a factor of two doubles.
        FUNCTION()
        void MoveBy(uint32 Tween, ECS::FEntity Target, FVector3 Offset, float Duration);

        FUNCTION()
        void RotateBy(uint32 Tween, ECS::FEntity Target, FQuat Delta, float Duration);

        FUNCTION()
        void ScaleBy(uint32 Tween, ECS::FEntity Target, FVector3 Factor, float Duration);

        // A kick toward Punch that swings back and settles, rotation in degrees. Vibrato counts the swings.
        FUNCTION()
        void Punch(uint32 Tween, ECS::FEntity Target, ETweenTransformChannel Channel, FVector3 Punch, float Duration, int32 Vibrato, float Elasticity);

        // A decaying random wobble of up to Strength on each axis, rotation in degrees.
        FUNCTION()
        void Shake(uint32 Tween, ECS::FEntity Target, ETweenTransformChannel Channel, FVector3 Strength, float Duration, int32 Vibrato);

        /** Interpolates a bare number, reporting each value to OnValue until the tween is destroyed. */
        FUNCTION()
        void ValueTo(uint32 Tween, float From, float To, float Duration, SScriptCallback OnValue);

        // The payload is the address of the value, read by the callback before it returns.
        FUNCTION()
        void ValueToVector2(uint32 Tween, FVector2 From, FVector2 To, float Duration, SScriptCallback OnValue);

        FUNCTION()
        void ValueToVector3(uint32 Tween, FVector3 From, FVector3 To, float Duration, SScriptCallback OnValue);

        FUNCTION()
        void ValueToVector4(uint32 Tween, FVector4 From, FVector4 To, float Duration, SScriptCallback OnValue);

        // Slerps, so a wide turn follows the arc.
        FUNCTION()
        void ValueToQuat(uint32 Tween, FQuat From, FQuat To, float Duration, SScriptCallback OnValue);

        // Drives a reflected property on Target's component, named by type and a dotted path, with no script call per frame.
        FUNCTION()
        void PropertyToFloat(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, float To, float Duration);

        FUNCTION()
        void PropertyToVector2(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FVector2 To, float Duration);

        FUNCTION()
        void PropertyToVector3(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FVector3 To, float Duration);

        FUNCTION()
        void PropertyToVector4(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FVector4 To, float Duration);

        FUNCTION()
        void PropertyToQuat(uint32 Tween, ECS::FEntity Target, const FString& Component, const FString& Path, FQuat To, float Duration);

        /** Dead time, for spacing steps apart. */
        FUNCTION()
        void Interval(uint32 Tween, float Duration);

        /** Runs OnCall when the sequence reaches this step. */
        FUNCTION()
        void Call(uint32 Tween, SScriptCallback OnCall);

        /** Runs OnFinished after the last step, including after the final loop. */
        FUNCTION()
        void OnFinished(uint32 Tween, SScriptCallback OnFinished);

        // Runs as each pass ends and another begins, with the passes completed so far as the payload.
        FUNCTION()
        void OnLoop(uint32 Tween, SScriptCallback OnLoop);

        // Runs as each step ends, with that step's index as the payload.
        FUNCTION()
        void OnStep(uint32 Tween, SScriptCallback OnStep);

        /** Transition, Ease and Delay all apply to the step that was added last. */
        FUNCTION()
        void Trans(uint32 Tween, EEaseTransition Transition);

        FUNCTION()
        void Ease(uint32 Tween, EEaseType Ease);

        // Shapes the last step with the curve over its keyed time range, replacing Trans and Ease.
        FUNCTION()
        void EaseCurve(uint32 Tween, CCurveAsset* Curve);

        FUNCTION()
        void Delay(uint32 Tween, float Seconds);

        /** Runs the last step alongside the one before it rather than after it. */
        FUNCTION()
        void Parallel(uint32 Tween);

        /** Zero or less loops forever. */
        FUNCTION()
        void SetLoops(uint32 Tween, int32 Count);

        FUNCTION()
        void SetLoopMode(uint32 Tween, ETweenLoopMode Mode);

        FUNCTION()
        void SetSpeedScale(uint32 Tween, float Scale);

        // Runs on real time, through world pause and time dilation.
        FUNCTION()
        void SetIgnoreTimeScale(uint32 Tween, bool bIgnore);

        FUNCTION()
        void SetPaused(uint32 Tween, bool bPaused);

        /** Stops where it is; whatever it was driving keeps its current value. */
        FUNCTION()
        void Kill(uint32 Tween);

        // Jumps to the end of the current pass, running every step and OnFinished on the way.
        FUNCTION()
        void Complete(uint32 Tween);

        FUNCTION()
        bool IsRunning(uint32 Tween);

        // Seconds into the current pass.
        FUNCTION()
        float GetElapsed(uint32 Tween);

        // The length of one pass.
        FUNCTION()
        float GetDuration(uint32 Tween);

        FUNCTION()
        float GetProgress(uint32 Tween);

        FUNCTION()
        int32 GetCompletedLoops(uint32 Tween);

        void OnTeardown() override;

        // The world's resource, so CTweenSystem and this subsystem share one instance.
        FTweenManager& GetTweenManager() { return *EnsureTweenManager(); }

    private:

        // Rebuilt per call, since the builder is just the manager plus a generational id.
        FTween Resolve(uint32 Tween);

        FTweenManager* EnsureTweenManager() const;
    };
}
