#include <gtest/gtest.h>

#include "Animation/AnimMontage.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

using namespace Lumina;

namespace
{
    CAnimationMontage* MakeMontage(FName Slot, float Duration)
    {
        CAnimation* Clip = NewObject<CAnimation>();
        Clip->GetAnimationResource()->Duration = Duration;

        CAnimationMontage* Montage = NewObject<CAnimationMontage>();
        Montage->BlendInTime  = 0.1f;
        Montage->BlendOutTime = 0.2f;

        SAnimMontageSlotTrack& Track = Montage->SlotTracks.EmplaceGetRef();
        Track.SlotName = Slot;
        Track.Segments.EmplaceGetRef().Animation = Clip;
        return Montage;
    }

    void Advance(FAnimMontagePlayer& Player, float Seconds, float Step = 1.0f / 60.0f)
    {
        for (float Time = 0.0f; Time < Seconds; Time += Step)
        {
            Player.Update(Step, nullptr);
        }
    }

    void ExpectTransition(const FAnimMontageTransition& Transition, const CAnimationMontage* Montage, EAnimMontageTransition Type, bool bInterrupted)
    {
        EXPECT_EQ(Transition.Montage.Get(), Montage);
        EXPECT_EQ(Transition.Type, Type);
        EXPECT_EQ(Transition.bInterrupted, bInterrupted);
    }
}

TEST(AnimMontageEvents, PlayedThroughReportsStartBlendOutAndEndUninterrupted)
{
    CAnimationMontage* Montage = MakeMontage("DefaultSlot", 1.0f);
    FAnimMontagePlayer Player;
    Player.Play(Montage);

    ASSERT_EQ(Player.Transitions.Num(), 1);
    ExpectTransition(Player.Transitions[0], Montage, EAnimMontageTransition::Started, false);

    Advance(Player, 0.7f);
    EXPECT_EQ(Player.Transitions.Num(), 1);

    Advance(Player, 0.15f);
    ASSERT_EQ(Player.Transitions.Num(), 2);
    ExpectTransition(Player.Transitions[1], Montage, EAnimMontageTransition::BlendingOut, false);
    EXPECT_GT(Player.GetWeight(Montage), 0.0f);

    Advance(Player, 0.5f);
    ASSERT_EQ(Player.Transitions.Num(), 3);
    ExpectTransition(Player.Transitions[2], Montage, EAnimMontageTransition::Ended, false);
    EXPECT_FALSE(Player.HasActive());
}

TEST(AnimMontageEvents, StopReportsInterruptedOnce)
{
    CAnimationMontage* Montage = MakeMontage("DefaultSlot", 1.0f);
    FAnimMontagePlayer Player;
    Player.Play(Montage);
    Advance(Player, 0.2f);

    Player.Stop(Montage);
    Player.Stop(Montage);
    Advance(Player, 0.5f);

    ASSERT_EQ(Player.Transitions.Num(), 3);
    ExpectTransition(Player.Transitions[1], Montage, EAnimMontageTransition::BlendingOut, true);
    ExpectTransition(Player.Transitions[2], Montage, EAnimMontageTransition::Ended, true);
}

TEST(AnimMontageEvents, MontageTakingTheSlotInterruptsTheOldOne)
{
    CAnimationMontage* First  = MakeMontage("UpperBody", 1.0f);
    CAnimationMontage* Second = MakeMontage("UpperBody", 1.0f);
    CAnimationMontage* Other  = MakeMontage("FullBody", 1.0f);
    FAnimMontagePlayer Player;
    Player.Play(First);
    Player.Play(Other);
    Advance(Player, 0.2f);
    Player.Transitions.Reset();

    Player.Play(Second);

    ASSERT_EQ(Player.Transitions.Num(), 2);
    ExpectTransition(Player.Transitions[0], First, EAnimMontageTransition::BlendingOut, true);
    ExpectTransition(Player.Transitions[1], Second, EAnimMontageTransition::Started, false);

    Advance(Player, 0.3f);
    ASSERT_EQ(Player.Transitions.Num(), 3);
    ExpectTransition(Player.Transitions[2], First, EAnimMontageTransition::Ended, true);
    EXPECT_TRUE(Player.IsPlaying(Other));
}

TEST(AnimMontageEvents, SectionJumpDispatchesNotifyInsideTheOvershootWindow)
{
    CAnimationMontage* Montage = MakeMontage("DefaultSlot", 2.0f);

    SAnimMontageSection& SectionA = Montage->Sections.EmplaceGetRef();
    SectionA.Name        = "A";
    SectionA.StartTime   = 0.0f;
    SectionA.NextSection = "B";

    SAnimMontageSection& SectionB = Montage->Sections.EmplaceGetRef();
    SectionB.Name      = "B";
    SectionB.StartTime = 1.0f;

    SAnimMontageNotify& Notify = Montage->Notifies.EmplaceGetRef();
    Notify.Name = "Hit";
    Notify.Time = 1.03f;

    FAnimMontagePlayer Player;
    Player.Play(Montage);
    Advance(Player, 0.9f);

    TVector<FAnimNotifyEvent> Events;
    Player.Update(0.15f, &Events);

    ASSERT_EQ(Events.Num(), 1);
    EXPECT_EQ(Events[0].Name, FName("Hit"));
    EXPECT_EQ(Events[0].Type, EAnimNotifyEventType::Trigger);
}

TEST(AnimMontageEvents, ResetEndsActiveMontagesAsInterrupted)
{
    CAnimationMontage* Montage = MakeMontage("DefaultSlot", 1.0f);
    FAnimMontagePlayer Player;
    Player.Play(Montage);
    Player.Transitions.Reset();

    Player.Reset();

    ASSERT_EQ(Player.Transitions.Num(), 1);
    ExpectTransition(Player.Transitions[0], Montage, EAnimMontageTransition::Ended, true);
}
