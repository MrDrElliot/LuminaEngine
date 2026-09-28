#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "SequenceLibrary.h"

#include "World/World.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "World/Entity/Systems/CameraSystem.h"

namespace Lumina
{
    ECS::FEntity CSequenceLibrary::PlaySequence(CWorld* World, CSequence* Sequence, bool bLoop, float PlayRate)
    {
        if (World == nullptr || Sequence == nullptr)
        {
            return ECS::NullEntity;
        }

        const ECS::FEntity Entity = World->ConstructEntity("SequencePlayer");
        SSequencePlayerComponent& Player = World->GetOrEmplaceComponent<SSequencePlayerComponent>(Entity);
        Player.Sequence = Sequence;
        Player.bLoop = bLoop;
        Player.PlayRate = PlayRate > 0.0f ? PlayRate : 1.0f;
        Player.Play(true);
        return Entity;
    }

    void CSequenceLibrary::StopSequence(CWorld* World, ECS::FEntity Player)
    {
        if (World == nullptr || !World->IsValidEntity(Player))
        {
            return;
        }

        if (SSequencePlayerComponent* Component = World->TryGetComponent<SSequencePlayerComponent>(Player))
        {
            Component->Instance.Release(World, Component->FinishAction == ESequenceFinishAction::Restore);
            Component->bPlaying = false;
        }
        World->DestroyEntity(Player);
    }

    bool CSequenceLibrary::IsSequencePlaying(CWorld* World, ECS::FEntity Player)
    {
        if (World == nullptr || !World->IsValidEntity(Player))
        {
            return false;
        }

        const SSequencePlayerComponent* Component = World->TryGetComponent<SSequencePlayerComponent>(Player);
        return Component != nullptr && Component->bPlaying;
    }

    void CSequenceLibrary::ConsumeEvents(CWorld* World, TVector<SSequenceFiredEvent>& OutEvents)
    {
        if (World == nullptr)
        {
            return;
        }

        auto View = World->View<SSequencePlayerComponent>();
        for (ECS::FEntity Entity : View)
        {
            TVector<SSequenceFiredEvent>& Fired = View.Get<SSequencePlayerComponent>(Entity).Instance.FiredEvents;
            for (SSequenceFiredEvent& Event : Fired)
            {
                OutEvents.push_back(Move(Event));
            }
            Fired.clear();
        }
    }

    namespace
    {
        struct FSequenceEventCursor
        {
            TVector<SSequenceFiredEvent> Pending;
            size_t                       Next = 0;
            SSequenceFiredEvent          Current;
        };

        FSequenceEventCursor& EventCursor(CWorld* World)
        {
            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
            if (FSequenceEventCursor* Cursor = Registry.Ctx().Find<FSequenceEventCursor>())
            {
                return *Cursor;
            }
            return Registry.Ctx().Emplace<FSequenceEventCursor>();
        }
    }

    bool CSequenceLibrary::NextEvent(CWorld* World)
    {
        if (World == nullptr)
        {
            return false;
        }

        FSequenceEventCursor& Cursor = EventCursor(World);
        if (Cursor.Next >= Cursor.Pending.size())
        {
            Cursor.Pending.clear();
            Cursor.Next = 0;
            ConsumeEvents(World, Cursor.Pending);
        }
        if (Cursor.Next >= Cursor.Pending.size())
        {
            Cursor.Current = SSequenceFiredEvent();
            return false;
        }

        Cursor.Current = Move(Cursor.Pending[Cursor.Next++]);
        return true;
    }

    FString CSequenceLibrary::GetEventName(CWorld* World)
    {
        return World != nullptr ? FString(EventCursor(World).Current.Name.ToString().c_str()) : FString();
    }

    FString CSequenceLibrary::GetEventPayload(CWorld* World)
    {
        return World != nullptr ? EventCursor(World).Current.Payload : FString();
    }

    ECS::FEntity CSequenceLibrary::GetEventTarget(CWorld* World)
    {
        return World != nullptr ? EventCursor(World).Current.Target : ECS::NullEntity;
    }

    float CSequenceLibrary::GetEventTime(CWorld* World)
    {
        return World != nullptr ? EventCursor(World).Current.Time : 0.0f;
    }

    bool CSequenceLibrary::IsAnySequencePlaying(CWorld* World)
    {
        if (World == nullptr)
        {
            return false;
        }

        auto View = World->View<SSequencePlayerComponent>();
        for (ECS::FEntity Entity : View)
        {
            if (View.Get<SSequencePlayerComponent>(Entity).bPlaying)
            {
                return true;
            }
        }
        return false;
    }

    void CSequenceLibrary::SetCinematicOverlay(CWorld* World, float Fade, FVector3 Color, float LetterboxAspect)
    {
        if (World != nullptr)
        {
            SCameraSystem::SetCinematicOverlay(ECS::GetWorldRegistry(*World), Fade, Color, LetterboxAspect);
        }
    }
}
