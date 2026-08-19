#pragma once
#include "Animation/AnimEvents.h"
#include "Animation/RootMotion.h"
#include "Animation/RootMotionTypes.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "SimpleAnimationComponent.generated.h"

namespace Lumina
{
    class CAnimation;

    REFLECT(Component, Category = "Animation")
    struct SSimpleAnimationComponent
    {
        GENERATED_BODY()

        /** The animation asset to play on the skeletal mesh. */
        PROPERTY(Editable, Replicated, Category = "Animation")
        TObjectPtr<CAnimation> Animation;

        /** Current playback position within the animation (seconds). */
        PROPERTY(Editable, Category = "Animation")
        float CurrentTime = 0.0f;

        /** Playback rate multiplier (1.0 = normal speed, 2.0 = double speed). */
        PROPERTY(Editable, Replicated, Category = "Animation")
        float PlaybackSpeed = 1.0f;

        /** When true, the animation restarts automatically upon completion. */
        PROPERTY(Editable, Replicated, Category = "Animation")
        bool bLooping = true;

        /** When true, the animation is currently advancing. */
        PROPERTY(Editable, Replicated, Category = "Animation")
        bool bPlaying = true;

        /** Overrides the active animation asset's root-motion lock for this entity (see ERootMotionLockMode). */
        PROPERTY(Editable, Category = "Animation")
        ERootMotionLockMode RootMotionLock = ERootMotionLockMode::FromAsset;

        // Root-motion delta extracted this frame in the parallel pass; applied to the entity transform
        // in the serial pass (transform writes mutate the registry and aren't ParallelFor-safe). Transient.
        FRootMotionDelta PendingRootMotion;

        /**
         * Set by the animation system the frame after a non-looping animation
         * completes. Stays true until the next PlayAnimation() call. Lets game
         * code react to "the punch finished" without manually polling time.
         */
        PROPERTY(Category = "Animation")
        bool bFinished = false;

        // Forces a fresh pose sample even when not playing (e.g. just-called PlayAnimation or rewound
        // CurrentTime). Cleared by the system after sampling.
        bool bDirty = true;

        // CurrentTime before this frame's advance, used to detect playhead crossings.
        float PreviousTime = 0.0f;

        // Set when CurrentTime advanced via playback (not a scrub/stop jump). Point notifies + NotifyState
        // Tick fire only when true, so Stop()/seek don't re-fire events. End still fires on stop.
        bool bAdvancedThisFrame = false;

        // Indices (into the active clip's NotifyStates) currently under the playhead.
        // Diffed each frame to drive Begin/End transitions; cleared on clip change.
        TVector<int32> ActiveNotifyStates;

        // Notify events that fired this frame: point Triggers plus notify-state Begin/Tick/End.
        // Cleared every update. Transient.
        TVector<FAnimNotifyEvent> NotifyEvents;

        /** True if the named point notify fired this frame. */
        FUNCTION()
        bool WasNotifyTriggered(const FName& NotifyName) const
        {
            for (const FAnimNotifyEvent& Event : NotifyEvents)
            {
                if (Event.Name == NotifyName && Event.Type == EAnimNotifyEventType::Trigger)
                {
                    return true;
                }
            }
            return false;
        }

        /** True while the playhead is inside the named notify-state window. */
        FUNCTION()
        bool IsNotifyStateActive(const FName& NotifyName) const;

        /** Value of the active clip's named curve at the current playhead, or Default if it has none. */
        FUNCTION()
        float GetCurveValue(const FName& CurveName, float Default = 0.0f) const;

        /**
         * Fire-and-forget play. Resets time to 0, marks the clip dirty, and
         * starts advancing. Pass bLoop=false for one-shot animations -- the
         * system stops playback and sets bFinished=true on completion.
         *
         * Calling this with the same clip restarts it from the beginning;
         * passing nullptr stops playback and clears the active clip.
         */
        FUNCTION()
        void PlayAnimation(CAnimation* InAnimation, bool bLoop = false, float Speed = 1.0f)
        {
            Animation       = InAnimation;
            CurrentTime     = 0.0f;
            PreviousTime    = 0.0f;
            PlaybackSpeed   = Speed;
            bLooping        = bLoop;
            bPlaying        = (InAnimation != nullptr);
            bFinished       = false;
            bDirty          = true;
            ActiveNotifyStates.clear();
        }

        /** Stop playback and freeze the pose at the current time. */
        FUNCTION()
        void Pause()
        {
            bPlaying = false;
        }

        /** Resume playback from the current time without resetting it. */
        FUNCTION()
        void Resume()
        {
            if (Animation.IsValid())
            {
                bPlaying  = true;
                bFinished = false;
            }
        }

        /**
         * Stop playback and snap back to time 0. Leaves the pose in its bind
         * pose on the next sample so the mesh visibly returns to rest.
         */
        FUNCTION()
        void Stop()
        {
            bPlaying    = false;
            CurrentTime = 0.0f;
            bFinished   = false;
            bDirty      = true;
        }

        /** True while a non-looping clip has run to completion. */
        FUNCTION()
        bool IsFinished() const { return bFinished; }

        /** True while the component is actively advancing the animation. */
        FUNCTION()
        bool IsPlaying() const { return bPlaying; }
    };
}
