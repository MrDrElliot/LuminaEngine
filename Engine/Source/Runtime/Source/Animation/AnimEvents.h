#pragma once

#include "World/ECS/Registry.h"


#include "Containers/Vector.h"
#include "Containers/Name.h"

namespace Lumina
{
    class CAnimation;
    struct SAnimNotify;
    struct SAnimNotifyState;

    enum class EAnimNotifyEventType : uint8
    {
        // Point notify crossed by the playhead this frame.
        Trigger,
        // Notify-state window entered / still under the playhead / left (simple playback only).
        Begin,
        Tick,
        End,
    };

    // One fired animation event, buffered per component for the frame it fired in.
    struct FAnimNotifyEvent
    {
        FName Name;
        FName Track;
        EAnimNotifyEventType Type = EAnimNotifyEventType::Trigger;

        // Blend weight of the branch that sampled the event (1 for direct clip playback). Graph
        // blends scale this; events from inactive state-machine branches never fire.
        float Weight = 1.0f;

        // Position inside a notify-state window, 0..1, for Tick.
        float Alpha = 0.0f;

        // The clip the notify was authored on.
        const CAnimation* Animation = nullptr;

        // Authored instance, null when the entry is name-only; points into asset data, valid this frame only.
        const SAnimNotify* Notify = nullptr;
        const SAnimNotifyState* State = nullptr;
    };

    namespace AnimEvents
    {
        // Appends a Trigger event for every point notify crossed in (PrevTime, CurTime], handling a
        // single loop wrap when CurTime landed behind PrevTime. Equal times append nothing.
        RUNTIME_API void CollectTriggeredNotifies(const CAnimation* Clip, float PrevTime, float CurTime,
                                                  bool bLooping, float Weight, TVector<FAnimNotifyEvent>& Out);

        // Runs the typed notify on every event that carries one. Serial pass only: these call user code.
        RUNTIME_API void DispatchTypedNotifies(const TVector<FAnimNotifyEvent>& Events, ECS::FRegistry& Registry, ECS::FEntity Entity);
    }
}
