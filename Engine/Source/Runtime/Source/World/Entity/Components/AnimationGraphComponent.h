#pragma once

#include "Animation/AnimationGraphVM.h"
#include "Animation/AnimEvents.h"
#include "Animation/AnimMontage.h"
#include "Animation/RootMotion.h"
#include "Animation/RootMotionTypes.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimationGraphComponent.generated.h"

namespace Lumina
{
    class CAnimationGraph;

    // Drives a skeletal mesh from a compiled animation graph.
    REFLECT(Component, Category = "Animation")
    struct RUNTIME_API SAnimationGraphComponent
    {
        GENERATED_BODY()

        /** Compiled animation graph asset evaluated each frame for this entity. */
        PROPERTY(Editable, Replicated, Category = "Animation")
        TObjectPtr<CAnimationGraph> Graph;

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

        // Brings Parameters up as an instance of the graph's struct, copying the graph's authored values.
        void EnsureParametersInitialized() const;

        /**
         * Root-motion policy for the graph's final pose. FromAsset extracts motion from clips with
         * root motion enabled, blends it through the graph, and drives the entity transform with it.
         * ForceLock pins the root without extraction; ForceUnlock leaves the root free in the pose.
         */
        PROPERTY(Editable, Category = "Animation")
        ERootMotionLockMode RootMotionLock = ERootMotionLockMode::FromAsset;

        // Per-instance VM state (registers, playback clocks, parameter values). Sized lazily from Graph; not serialized.
        FAnimGraphVMState VMState;

        // This frame's blended root-motion delta, extracted in the parallel update and applied to the
        // entity transform in the serial pass. Transient.
        FRootMotionDelta PendingRootMotion;

        // Notify events that fired this frame from the graph's active branches, weighted by their
        // branch's blend alpha. Cleared every update. Transient.
        TVector<FAnimNotifyEvent> NotifyEvents;

        // Montages playing on this entity, layered into the graph by its Slot nodes. Transient.
        FAnimMontagePlayer Montages;

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
            return std::ranges::any_of(NotifyEvents, [&NotifyName](const FAnimNotifyEvent& Event)
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

        // Curve values are produced by the graph's own evaluation (clips carry the keys, blends weight
        // them), so they are read-only here: the last evaluated frame's value of the output pose.

        /** Value the named animation curve carried into the output pose this frame. */
        FUNCTION()
        float GetCurveValue(const FName& CurveName, float Default = 0.0f) const;

        /** True if the graph carries a curve slot with the given name. */
        FUNCTION()
        bool HasCurve(const FName& CurveName) const;

        // Sizes VMState from the current graph if it has not been initialized
        // yet, so Lua / the system can set parameters before the VM's first tick.
        void EnsureStateInitialized();
    };
}
