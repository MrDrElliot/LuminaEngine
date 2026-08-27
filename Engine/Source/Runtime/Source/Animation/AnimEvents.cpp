#include "RuntimePCH.h"
#include "AnimEvents.h"
#include "World/ECS/Registry.h"

#include "AnimNotify.h"
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
            Event.Name      = Notify.NotifyName;
            Event.Track     = Notify.NotifyTrack;
            Event.Type      = EAnimNotifyEventType::Trigger;
            Event.Weight    = Weight;
            Event.Alpha     = 0.0f;
            Event.Animation = Clip;
            Event.Notify    = Notify.Notify.Get();
        }
    }

    void AnimEvents::DispatchTypedNotifies(const TVector<FAnimNotifyEvent>& Events, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        for (const FAnimNotifyEvent& Event : Events)
        {
            switch (Event.Type)
            {
            case EAnimNotifyEventType::Trigger:
                if (Event.Notify != nullptr)
                {
                    Event.Notify->Notify(Registry, Entity);
                }
                break;
            case EAnimNotifyEventType::Begin:
                if (Event.State != nullptr)
                {
                    Event.State->NotifyBegin(Registry, Entity);
                }
                break;
            case EAnimNotifyEventType::Tick:
                if (Event.State != nullptr)
                {
                    Event.State->NotifyTick(Registry, Entity, Event.Alpha);
                }
                break;
            case EAnimNotifyEventType::End:
                if (Event.State != nullptr)
                {
                    Event.State->NotifyEnd(Registry, Entity);
                }
                break;
            }
        }
    }
}
