#include "pch.h"
#include "AnimEvents.h"

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    void AnimEvents::CollectTriggeredNotifies(const CAnimation* Clip, float PrevTime, float CurTime,
                                              bool bLooping, float Weight, TVector<FAnimNotifyEvent>& Out)
    {
        if (Clip == nullptr || PrevTime == CurTime)
        {
            return;
        }

        const TVector<FAnimationNotify>& Notifies = Clip->GetNotifies();
        if (Notifies.empty())
        {
            return;
        }

        const bool bWrapped = CurTime < PrevTime && bLooping;

        for (const FAnimationNotify& Notify : Notifies)
        {
            const float T = Notify.Time;
            const bool bCrossed = bWrapped
                ? (T > PrevTime || T <= CurTime)
                : (T > PrevTime && T <= CurTime);
            if (!bCrossed)
            {
                continue;
            }

            FAnimNotifyEvent& Event = Out.emplace_back();
            Event.Name   = Notify.NotifyName;
            Event.Track  = Notify.NotifyTrack;
            Event.Type   = EAnimNotifyEventType::Trigger;
            Event.Weight = Weight;
        }
    }
}
