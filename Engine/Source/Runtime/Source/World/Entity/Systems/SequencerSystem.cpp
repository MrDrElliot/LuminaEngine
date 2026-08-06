#include "RuntimePCH.h"
#include "SequencerSystem.h"

#include "World/World.h"
#include "World/Entity/Components/SequencePlayerComponent.h"

namespace Lumina
{
    void SSequencerSystem::Update(const FSystemContext& SystemContext) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* World = SystemContext.GetWorld();
        if (World == nullptr)
        {
            return;
        }

        const float DeltaTime = (float)SystemContext.GetDeltaTime();

        auto View = SystemContext.CreateView<SSequencePlayerComponent>();
        for (entt::entity Entity : View)
        {
            SSequencePlayerComponent& Player = View.get<SSequencePlayerComponent>(Entity);

            CSequence* Sequence = Player.Sequence.Get();

            if (Player.bAutoPlay && !Player.bPlaying && !Player.Instance.bBound && Sequence != nullptr)
            {
                Player.bPlaying = true;
                Player.Time = 0.0f;
                Player.bAutoPlay = false;
            }

            // The asset being swapped underneath a live instance would leave bindings pointing at the old
            // sequence's table, so a change tears down before anything else looks at it.
            if (Player.Instance.bBound && Player.BoundSequence != Sequence)
            {
                Player.Instance.Release(World, Player.FinishAction == ESequenceFinishAction::Restore);
                Player.BoundSequence = nullptr;
            }

            if (!Player.bPlaying)
            {
                if (Player.Instance.bBound)
                {
                    Player.Instance.Release(World, Player.FinishAction == ESequenceFinishAction::Restore);
                    Player.BoundSequence = nullptr;
                }
                continue;
            }

            if (Sequence == nullptr)
            {
                Player.bPlaying = false;
                continue;
            }

            if (!Player.Instance.bBound)
            {
                Player.Instance.Bind(Sequence, World);
                Player.BoundSequence = Sequence;
                Player.PreviousTime = Player.Time;
                Player.bJumped = true;
            }

            const float PreviousTime = Player.PreviousTime;

            float NewTime = Player.Time + DeltaTime * Player.PlayRate;
            bool bJumped = Player.bJumped;
            bool bFinished = false;

            if (NewTime >= Sequence->Duration)
            {
                if (Player.bLoop)
                {
                    NewTime = Sequence->Duration > 0.0f ? fmodf(NewTime, Sequence->Duration) : 0.0f;
                    bJumped = true;
                }
                else
                {
                    NewTime = Sequence->Duration;
                    bFinished = true;
                }
            }

            Player.Instance.Evaluate(Sequence, World, NewTime, PreviousTime, bJumped);

            Player.PreviousTime = NewTime;
            Player.Time = NewTime;
            Player.bJumped = false;

            // Evaluated at the final frame first, then torn down: stopping a frame early would leave the
            // shot one frame short of the pose it was authored to end on.
            if (bFinished)
            {
                Player.bPlaying = false;
                Player.Instance.Release(World, Player.FinishAction == ESequenceFinishAction::Restore);
                Player.BoundSequence = nullptr;
            }
        }
    }
}
