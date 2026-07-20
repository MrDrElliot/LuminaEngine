#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"

namespace Lumina
{
    class CAnimation;

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
    };

    namespace AnimEvents
    {
        // Appends a Trigger event for every point notify crossed in (PrevTime, CurTime], handling a
        // single loop wrap when CurTime landed behind PrevTime. Equal times append nothing.
        RUNTIME_API void CollectTriggeredNotifies(const CAnimation* Clip, float PrevTime, float CurTime,
                                                  bool bLooping, float Weight, TVector<FAnimNotifyEvent>& Out);
    }
}
