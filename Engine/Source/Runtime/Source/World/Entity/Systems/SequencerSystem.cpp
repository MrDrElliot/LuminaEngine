#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "SequencerSystem.h"

#include "World/World.h"
#include "World/Entity/Components/SequencePlayerComponent.h"

namespace Lumina
{
    void SSequencerSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics, EUpdatePriority::High);
    }

    bool SSequencerSystem::HasWork(EUpdateStage)
    {
        CWorld* World = GetWorld();
        const ECS::TComponentStorage<SSequencePlayerComponent> Players = World != nullptr
            ? ECS::GetWorldRegistry(*World).FindStorage<SSequencePlayerComponent>()
            : ECS::TComponentStorage<SSequencePlayerComponent>();
        return Players && !Players.IsEmpty();
    }

    void SSequencerSystem::OnUpdate()
    {
        const FSystemContext& SystemContext = GetContext();

        LUMINA_PROFILE_SCOPE();

        CWorld* World = SystemContext.GetWorld();
        if (World == nullptr)
        {
            return;
        }

        // Real time, so a time dilation track slows the action on screen without stretching the edit.
        const float DeltaTime = (float)World->GetRealDeltaTime();

        auto View = SystemContext.CreateView<SSequencePlayerComponent>();
        for (ECS::FEntity Entity : View)
        {
            SSequencePlayerComponent& Player = View.Get<SSequencePlayerComponent>(Entity);

            CSequence* Sequence = Player.Sequence.Get();
            const bool bRestore = Player.FinishAction == ESequenceFinishAction::Restore;

            if (Player.bAutoPlay && !Player.bPlaying && !Player.Instance.bBound && Sequence != nullptr)
            {
                Player.bPlaying = true;
                Player.Time = 0.0f;
                Player.bAutoPlay = false;
            }

            // A swapped asset would leave bindings pointing at the old sequence's table.
            if (Player.Instance.bBound && Player.BoundSequence != Sequence)
            {
                Player.Instance.Release(World, bRestore);
                Player.BoundSequence = nullptr;
            }

            if (!Player.bPlaying)
            {
                if (Player.Instance.bBound)
                {
                    Player.Instance.Release(World, bRestore);
                    Player.BoundSequence = nullptr;
                }
                Player.bPaused = false;
                continue;
            }

            if (Sequence == nullptr)
            {
                Player.bPlaying = false;
                continue;
            }

            bool bFirstFrame = false;
            if (!Player.Instance.bBound)
            {
                Player.Instance.Bind(Sequence, World);
                Player.BoundSequence = Sequence;
                Player.PreviousTime = Player.Time;
                bFirstFrame = true;
            }

            if (Player.bPaused)
            {
                // Evaluated every paused frame, so sounds the sequence started hold with it instead of running on.
                const bool bRefresh = bFirstFrame || Player.bNeedsEvaluate;
                Player.Instance.Evaluate(Sequence, World, Player.Time, Player.Time, bRefresh, bFirstFrame, false, true);
                Player.bNeedsEvaluate = false;
                Player.bJumped = false;
                continue;
            }

            const float PreviousTime = Player.PreviousTime;

            float NewTime = Player.Time + DeltaTime * Player.PlayRate;
            const bool bJumped = Player.bJumped;
            bool bWrapped = false;
            bool bFinished = false;

            if (NewTime >= Sequence->Duration)
            {
                if (Player.bLoop)
                {
                    NewTime = Sequence->Duration > 0.0f ? fmodf(NewTime, Sequence->Duration) : 0.0f;
                    bWrapped = true;
                }
                else
                {
                    NewTime = Sequence->Duration;
                    bFinished = true;
                }
            }

            Player.Instance.Evaluate(Sequence, World, NewTime, PreviousTime, bJumped, bFirstFrame, bWrapped);

            Player.PreviousTime = NewTime;
            Player.Time = NewTime;
            Player.bJumped = false;
            Player.bNeedsEvaluate = false;

            // Evaluate the final frame before tearing down, or the shot ends one frame short.
            if (bFinished)
            {
                Player.bPlaying = false;
                Player.Instance.Release(World, bRestore);
                Player.BoundSequence = nullptr;
            }
        }
    }
}
