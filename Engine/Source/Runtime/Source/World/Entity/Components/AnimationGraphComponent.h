#pragma once

#include "Animation/AnimationGraphVM.h"
#include "Animation/AnimEvents.h"
#include "Animation/RootMotion.h"
#include "Animation/RootMotionTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimationGraphComponent.generated.h"

namespace Lumina
{
    class CAnimationGraph;

    // Drives a skeletal mesh from a compiled animation graph; SAnimationSystem runs the bytecode each
    // frame into SSkeletalMeshComponent. Graph parameters exposed to Lua via the Set/Get functions below.
    REFLECT(Component, Category = "Animation")
    struct SAnimationGraphComponent
    {
        GENERATED_BODY()

        /** Compiled animation graph asset evaluated each frame for this entity. */
        PROPERTY(Script, Editable, Replicated, Category = "Animation")
        TObjectPtr<CAnimationGraph> Graph;

        /**
         * Root-motion policy for the graph's final pose. FromAsset extracts motion from clips with
         * root motion enabled, blends it through the graph, and drives the entity transform with it.
         * ForceLock pins the root without extraction; ForceUnlock leaves the root free in the pose.
         */
        PROPERTY(Script, Editable, Category = "Animation")
        ERootMotionLockMode RootMotionLock = ERootMotionLockMode::FromAsset;

        // Per-instance VM state (registers, playback clocks, parameter values). Sized lazily from Graph; not serialized.
        FAnimGraphVMState VMState;

        // This frame's blended root-motion delta, extracted in the parallel update and applied to the
        // entity transform in the serial pass. Transient.
        FRootMotionDelta PendingRootMotion;

        // Notify events that fired this frame from the graph's active branches, weighted by their
        // branch's blend alpha. Cleared every update. Transient.
        TVector<FAnimNotifyEvent> NotifyEvents;

        /** True if the named point notify fired this frame (any weight). */
        FUNCTION(Script)
        bool WasNotifyTriggered(const FName& NotifyName) const
        {
            return std::ranges::any_of(NotifyEvents, [&NotifyName](const FAnimNotifyEvent& Event)
            {
                return Event.Name == NotifyName;
            });
        }

        // Parameter access below writes the VM's own register table. When the entity also has an
        // SBlackboardComponent, SAnimationSystem refills those registers from the blackboard before every
        // evaluation, so the blackboard - not this - is the value that survives. Write through the
        // blackboard (or World.Animation, which routes there automatically) on such entities.

        /** Sets a named float parameter. No-op if the graph has no such parameter. */
        FUNCTION(Script)
        void SetFloat(const FName& ParameterName, float Value);

        /** Returns a named float parameter, or Default if the graph has no such parameter. */
        FUNCTION(Script)
        float GetFloat(const FName& ParameterName, float Default = 0.0f) const;

        /** Sets a named bool parameter (stored as 0/1). No-op if the graph has no such parameter. */
        FUNCTION(Script)
        void SetBool(const FName& ParameterName, bool bValue);

        /** Returns a named bool parameter, or Default if the graph has no such parameter. */
        FUNCTION(Script)
        bool GetBool(const FName& ParameterName, bool Default = false) const;

        /** True if the graph declares a parameter with the given name. */
        FUNCTION(Script)
        bool HasParameter(const FName& ParameterName) const;

        // Curve values are produced by the graph's own evaluation (clips carry the keys, blends weight
        // them), so they are read-only here: the last evaluated frame's value of the output pose.

        /** Value the named animation curve carried into the output pose this frame. */
        FUNCTION(Script)
        float GetCurveValue(const FName& CurveName, float Default = 0.0f) const;

        /** True if the graph carries a curve slot with the given name. */
        FUNCTION(Script)
        bool HasCurve(const FName& CurveName) const;

        // Sizes VMState from the current graph if it has not been initialized
        // yet, so Lua / the system can set parameters before the VM's first tick.
        void EnsureStateInitialized();
    };
}
