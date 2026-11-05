#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "World/Subsystems/SequenceSubsystem.h"

#include "World/World.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "World/Entity/Systems/CameraSystem.h"

namespace Lumina
{
    namespace
    {
        struct FSequenceEventCursor
        {
            TVector<SSequenceFiredEvent> Pending;
            int32                        Next = 0;
            SSequenceFiredEvent          Current;
        };

        FSequenceEventCursor& EventCursor(CWorld* World)
        {
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            if (FSequenceEventCursor* Cursor = Registry.Ctx().Find<FSequenceEventCursor>())
            {
                return *Cursor;
            }
            return Registry.Ctx().Emplace<FSequenceEventCursor>();
        }
    }

    ECS::FEntity CSequenceSubsystem::PlaySequence(CSequence* Sequence, bool bLoop, float PlayRate)
    {
        CWorld* World = GetWorld();
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

    void CSequenceSubsystem::StopSequence(ECS::FEntity Player)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || !World->IsValidEntity(Player))
        {
            return;
        }

        if (SSequencePlayerComponent* Component = World->TryGetComponent<ECS::Write<SSequencePlayerComponent>>(Player))
        {
            Component->Instance.Release(World, Component->FinishAction == ESequenceFinishAction::Restore);
            Component->bPlaying = false;
        }
        World->DestroyEntity(Player);
    }

    bool CSequenceSubsystem::IsSequencePlaying(ECS::FEntity Player)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || !World->IsValidEntity(Player))
        {
            return false;
        }

        const SSequencePlayerComponent* Component = World->TryGetComponent<ECS::Read<SSequencePlayerComponent>>(Player);
        return Component != nullptr && Component->bPlaying;
    }

    void CSequenceSubsystem::ConsumeEvents(TVector<SSequenceFiredEvent>& OutEvents)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return;
        }

        auto View = World->View<ECS::Write<SSequencePlayerComponent>>();
        for (ECS::FEntity Entity : View)
        {
            TVector<SSequenceFiredEvent>& Fired = View.Get<ECS::Write<SSequencePlayerComponent>>(Entity).Instance.FiredEvents;
            for (SSequenceFiredEvent& Event : Fired)
            {
                OutEvents.Add(Move(Event));
            }
            Fired.Reset();
        }
    }

    bool CSequenceSubsystem::NextEvent()
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return false;
        }

        FSequenceEventCursor& Cursor = EventCursor(World);
        if (Cursor.Next >= Cursor.Pending.Num())
        {
            Cursor.Pending.Reset();
            Cursor.Next = 0;
            ConsumeEvents(Cursor.Pending);
        }
        if (Cursor.Next >= Cursor.Pending.Num())
        {
            Cursor.Current = SSequenceFiredEvent();
            return false;
        }

        Cursor.Current = Move(Cursor.Pending[Cursor.Next++]);
        return true;
    }

    FString CSequenceSubsystem::GetEventName()
    {
        CWorld* World = GetWorld();
        return World != nullptr ? FString(*EventCursor(World).Current.Name.ToString()) : FString();
    }

    FString CSequenceSubsystem::GetEventPayload()
    {
        CWorld* World = GetWorld();
        return World != nullptr ? EventCursor(World).Current.Payload : FString();
    }

    ECS::FEntity CSequenceSubsystem::GetEventTarget()
    {
        CWorld* World = GetWorld();
        return World != nullptr ? EventCursor(World).Current.Target : ECS::NullEntity;
    }

    float CSequenceSubsystem::GetEventTime()
    {
        CWorld* World = GetWorld();
        return World != nullptr ? EventCursor(World).Current.Time : 0.0f;
    }

    bool CSequenceSubsystem::IsAnySequencePlaying()
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return false;
        }

        auto View = World->View<ECS::Read<SSequencePlayerComponent>>();
        for (ECS::FEntity Entity : View)
        {
            if (View.Get<ECS::Read<SSequencePlayerComponent>>(Entity).bPlaying)
            {
                return true;
            }
        }
        return false;
    }

    void CSequenceSubsystem::SetCinematicOverlay(float Fade, FVector3 Color, float LetterboxAspect)
    {
        CWorld* World = GetWorld();
        if (World != nullptr)
        {
            SCameraSystem::SetCinematicOverlay(World->GetEntityRegistry(), Fade, Color, LetterboxAspect);
        }
    }
}
