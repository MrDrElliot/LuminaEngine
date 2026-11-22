#pragma once

#include "Animation/AnimationGraphVM.h"
#include "Animation/AnimEvents.h"
#include "Animation/AnimMontage.h"
#include "Animation/RootMotion.h"
#include "Animation/RootMotionTypes.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/Algorithm.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "World/ECS/EventMeta.h"
#include "AnimationGraphComponent.generated.h"

namespace Lumina
{
    class CAnimationGraph;

    // A notify crossed this frame by a simple or graph animator, queued for systems only while something reads it.
    REFLECT(Event)
    struct RUNTIME_API SAnimNotifyFiredEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        FName Name;

        PROPERTY(Hidden)
        FName Track;

        PROPERTY(Hidden)
        EAnimNotifyEventType Type = EAnimNotifyEventType::Trigger;

        // The weight of the blend branch that fired it, 1 for direct clip playback.
        PROPERTY(Hidden)
        float Weight = 1.0f;

        // Position inside a notify state's window, 0 to 1.
        PROPERTY(Hidden)
        float Alpha = 0.0f;
    };

    // A montage began playing, queued for systems and fanned out to the graph component's OnMontageStarted.
    REFLECT(Event)
    struct RUNTIME_API SAnimMontageStartedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        TStrongObjectPtr<CAnimationMontage> Montage;
    };

    // A montage started fading out, the moment to queue what plays next. Fanned out to OnMontageBlendingOut.
    REFLECT(Event)
    struct RUNTIME_API SAnimMontageBlendingOutEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        TStrongObjectPtr<CAnimationMontage> Montage;

        // Stopped or cut off by another montage on its slot, rather than played through to its end.
        PROPERTY(Hidden)
        bool bInterrupted = false;
    };

    // A montage finished and no longer affects the pose. Fanned out to OnMontageEnded.
    REFLECT(Event)
    struct RUNTIME_API SAnimMontageEndedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        TStrongObjectPtr<CAnimationMontage> Montage;

        PROPERTY(Hidden)
        bool bInterrupted = false;
    };

    REFLECT(Component, Category = "Animation")
    struct RUNTIME_API SAnimationGraphComponent
    {
        GENERATED_BODY()

        /** Compiled animation graph asset evaluated each frame for this entity. */
        PROPERTY(Editable, Replicated, Category = "Animation")
        TStrongObjectPtr<CAnimationGraph> Graph;

        /** Live parameter block, an instance of the graph's ParameterStruct. Write its fields directly. */
        PROPERTY(Editable, Category = "Animation")
        FInstancedStruct Parameters;

        /** Typed access to the parameter block; null when it isn't an instance of T. */
        template<InstancableStruct T>
        T* GetParameters()
        {
            EnsureParametersInitialized();
            return Parameters.GetMutablePtr<T>();
        }

        template<InstancableStruct T>
        const T* GetParameters() const
        {
            EnsureParametersInitialized();
            return Parameters.GetPtr<T>();
        }

        /** Raw parameter memory, for the anim system's offset reads. Null when no struct is assigned. */
        void* GetParameterMemory();

        // The parameter block when the graph names TypeName as its parameter struct, so a caller's mirror cannot misread it.
        FUNCTION(ScriptInternal)
        void* GetParameterMemoryAs(FName TypeName);

        // Brings Parameters up as an instance of the graph's struct, copying the graph's authored values.
        void EnsureParametersInitialized() const;

        /**
         * Root-motion policy for the graph's final pose. FromAsset extracts motion from clips with
         * root motion enabled, blends it through the graph, and drives the entity transform with it.
         * ForceLock pins the root without extraction; ForceUnlock leaves the root free in the pose.
         */
        PROPERTY(Editable, Category = "Animation")
        ERootMotionLockMode RootMotionLock = ERootMotionLockMode::FromAsset;

        // When false the graph holds its current pose, clocks and montages included.
        PROPERTY(Editable, Replicated, Category = "Animation")
        bool bPlaying = true;

        // Playback rate multiplier for the whole graph, 1 being normal speed.
        PROPERTY(Editable, Replicated, Category = "Animation")
        float PlaybackSpeed = 1.0f;

        // Per-instance VM state (registers, playback clocks, parameter values). Sized lazily from Graph; not serialized.
        FAnimGraphVMState VMState;

        // This frame's blended root-motion delta, applied in the serial pass. Transient.
        FRootMotionDelta PendingRootMotion;

        // Notify events from the graph's active branches this frame, cleared every update. Transient.
        TVector<FAnimNotifyEvent> NotifyEvents;

        // Montages playing on this entity, layered into the graph by its Slot nodes. Transient.
        FAnimMontagePlayer Montages;

        // Systems that hear every entity's montages read the SAnimMontage event queues instead.
        PROPERTY(Hidden)
        TScriptDelegate<SAnimMontageStartedEvent> OnMontageStarted;

        PROPERTY(Hidden)
        TScriptDelegate<SAnimMontageBlendingOutEvent> OnMontageBlendingOut;

        PROPERTY(Hidden)
        TScriptDelegate<SAnimMontageEndedEvent> OnMontageEnded;

        /** Plays a montage on the slots its tracks name, blending out anything already on them. */
        FUNCTION()
        void PlayMontage(CAnimationMontage* Montage, float PlayRate = 1.0f)
        {
            Montages.Play(Montage, PlayRate, FName());
        }

        /** Plays a montage starting at a named section. */
        FUNCTION()
        void PlayMontageFromSection(CAnimationMontage* Montage, const FName& SectionName, float PlayRate = 1.0f)
        {
            Montages.Play(Montage, PlayRate, SectionName);
        }

        /** Blends a montage out. A negative BlendOutTime uses the montage's own blend out time. */
        FUNCTION()
        void StopMontage(CAnimationMontage* Montage, float BlendOutTime = -1.0f)
        {
            Montages.Stop(Montage, BlendOutTime);
        }

        FUNCTION()
        void StopAllMontages(float BlendOutTime = -1.0f)
        {
            Montages.StopAll(BlendOutTime);
        }

        /** Moves a playing montage's playhead to a named section. False if it is not playing. */
        FUNCTION()
        bool JumpToMontageSection(CAnimationMontage* Montage, const FName& SectionName)
        {
            return Montages.JumpToSection(Montage, SectionName);
        }

        /** Overrides which section follows the one playing now, which is how combos chain. */
        FUNCTION()
        bool SetNextMontageSection(CAnimationMontage* Montage, const FName& SectionName)
        {
            return Montages.SetNextSection(Montage, SectionName);
        }

        /** True while the montage is playing and not already blending out. */
        FUNCTION()
        bool IsMontagePlaying(CAnimationMontage* Montage) const
        {
            return Montages.IsPlaying(Montage);
        }

        /** True while any montage is contributing to this entity's pose. */
        FUNCTION()
        bool IsAnyMontagePlaying() const
        {
            return Montages.HasActive();
        }

        FUNCTION()
        FName GetMontageSection(CAnimationMontage* Montage) const
        {
            return Montages.GetCurrentSection(Montage);
        }

        /** Playhead position on the montage timeline, in seconds. */
        FUNCTION()
        float GetMontagePosition(CAnimationMontage* Montage) const
        {
            return Montages.GetPosition(Montage);
        }

        /** How strongly the montage is currently blended over the graph pose. */
        FUNCTION()
        float GetMontageWeight(CAnimationMontage* Montage) const
        {
            return Montages.GetWeight(Montage);
        }

        FUNCTION()
        void SetMontagePlayRate(CAnimationMontage* Montage, float PlayRate)
        {
            Montages.SetPlayRate(Montage, PlayRate);
        }

        /** True if the named point notify fired this frame (any weight). */
        FUNCTION()
        bool WasNotifyTriggered(const FName& NotifyName) const
        {
            return Algo::AnyOf(NotifyEvents, [&NotifyName](const FAnimNotifyEvent& Event)
            {
                return Event.Name == NotifyName;
            });
        }

        /** True if the graph declares a parameter with the given name. */
        FUNCTION()
        bool HasParameter(const FName& ParameterName) const;

        /** Value the VM last evaluated this parameter at. Read-only; write the parameter struct instead. */
        FUNCTION()
        float GetEvaluatedParameter(const FName& ParameterName, float Default = 0.0f) const;

        // Read-only because the graph's own evaluation produces curve values, as of the last evaluated frame.

        /** Value the named animation curve carried into the output pose this frame. */
        FUNCTION()
        float GetCurveValue(const FName& CurveName, float Default = 0.0f) const;

        /** True if the graph carries a curve slot with the given name. */
        FUNCTION()
        bool HasCurve(const FName& CurveName) const;

        // Sizes VMState from the current graph if uninitialized, so parameters can be set before the first tick.
        void EnsureStateInitialized();
    };

    template<>
    struct TEventFanOut<SAnimMontageStartedEvent> : TEntityDelegateFanOut<SAnimMontageStartedEvent, &SAnimationGraphComponent::OnMontageStarted> {};

    template<>
    struct TEventFanOut<SAnimMontageBlendingOutEvent> : TEntityDelegateFanOut<SAnimMontageBlendingOutEvent, &SAnimationGraphComponent::OnMontageBlendingOut> {};

    template<>
    struct TEventFanOut<SAnimMontageEndedEvent> : TEntityDelegateFanOut<SAnimMontageEndedEvent, &SAnimationGraphComponent::OnMontageEnded> {};
}
