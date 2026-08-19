#include "RuntimePCH.h"
#include "AnimMontage.h"

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    namespace
    {
        float ResolveBlendRate(float BlendTime)
        {
            return BlendTime > 1e-4f ? (1.0f / BlendTime) : 1000.0f;
        }
    }

    uint32 FAnimMontagePlayer::Play(CAnimationMontage* Montage, float PlayRate, const FName& StartSection)
    {
        if (Montage == nullptr)
        {
            return 0;
        }

        // A slot can only show one montage, so anything sharing one fades out as this comes in.
        TVector<FName> Slots;
        Montage->GetSlotNames(Slots);

        for (FAnimMontageInstance& Existing : Instances)
        {
            if (!Existing.IsActive() || !Existing.Montage.IsValid())
            {
                continue;
            }

            TVector<FName> ExistingSlots;
            Existing.Montage->GetSlotNames(ExistingSlots);

            bool bOverlaps = false;
            for (const FName& Slot : ExistingSlots)
            {
                bOverlaps = bOverlaps || eastl::find(Slots.begin(), Slots.end(), Slot) != Slots.end();
            }

            if (bOverlaps)
            {
                BeginBlendOut(Existing, -1.0f);
            }
        }

        FAnimMontageInstance& Instance = Instances.emplace_back();
        Instance.Montage    = Montage;
        Instance.InstanceID = NextInstanceID++;
        Instance.PlayRate   = Math::Max(PlayRate, 0.0f);
        Instance.State      = EAnimMontageState::BlendingIn;
        Instance.BlendRate  = ResolveBlendRate(Montage->BlendInTime);

        int32 SectionIndex = StartSection.IsNone() ? INDEX_NONE : Montage->FindSectionIndex(StartSection);
        if (SectionIndex == INDEX_NONE && !Montage->Sections.empty())
        {
            SectionIndex = Montage->FindSectionAtTime(0.0f);
        }

        Instance.CurrentSection = SectionIndex;
        Instance.Position       = (SectionIndex != INDEX_NONE) ? Montage->Sections[SectionIndex].StartTime : 0.0f;
        Instance.PrevPosition   = Instance.Position;

        return Instance.InstanceID;
    }

    void FAnimMontagePlayer::Stop(const CAnimationMontage* Montage, float BlendOutTime)
    {
        if (FAnimMontageInstance* Instance = Find(Montage))
        {
            BeginBlendOut(*Instance, BlendOutTime);
        }
    }

    void FAnimMontagePlayer::StopAll(float BlendOutTime)
    {
        for (FAnimMontageInstance& Instance : Instances)
        {
            if (Instance.IsActive())
            {
                BeginBlendOut(Instance, BlendOutTime);
            }
        }
    }

    void FAnimMontagePlayer::Reset()
    {
        Instances.clear();
    }

    void FAnimMontagePlayer::BeginBlendOut(FAnimMontageInstance& Instance, float BlendOutTime)
    {
        if (Instance.State == EAnimMontageState::BlendingOut || Instance.State == EAnimMontageState::Finished)
        {
            return;
        }

        const float Duration = (BlendOutTime >= 0.0f)
            ? BlendOutTime
            : (Instance.Montage.IsValid() ? Instance.Montage->BlendOutTime : 0.0f);

        Instance.State     = EAnimMontageState::BlendingOut;
        Instance.BlendRate = ResolveBlendRate(Duration);
    }

    bool FAnimMontagePlayer::JumpToSection(const CAnimationMontage* Montage, const FName& SectionName)
    {
        FAnimMontageInstance* Instance = Find(Montage);
        if (Instance == nullptr || !Instance->Montage.IsValid())
        {
            return false;
        }

        const int32 SectionIndex = Instance->Montage->FindSectionIndex(SectionName);
        if (SectionIndex == INDEX_NONE)
        {
            return false;
        }

        Instance->CurrentSection = SectionIndex;
        Instance->Position       = Instance->Montage->Sections[SectionIndex].StartTime;
        Instance->PrevPosition   = Instance->Position;
        Instance->QueuedSection  = FName();

        // A jump out of a blend-out re-arms the montage, which is what a combo follow-up needs.
        if (Instance->State == EAnimMontageState::BlendingOut)
        {
            Instance->State     = EAnimMontageState::BlendingIn;
            Instance->BlendRate = ResolveBlendRate(Instance->Montage->BlendInTime);
        }

        return true;
    }

    bool FAnimMontagePlayer::SetNextSection(const CAnimationMontage* Montage, const FName& SectionName)
    {
        FAnimMontageInstance* Instance = Find(Montage);
        if (Instance == nullptr || !Instance->Montage.IsValid())
        {
            return false;
        }

        if (!SectionName.IsNone() && Instance->Montage->FindSectionIndex(SectionName) == INDEX_NONE)
        {
            return false;
        }

        Instance->QueuedSection = SectionName;
        return true;
    }

    bool FAnimMontagePlayer::IsPlaying(const CAnimationMontage* Montage) const
    {
        const FAnimMontageInstance* Instance = Find(Montage);
        return Instance != nullptr && Instance->State != EAnimMontageState::BlendingOut;
    }

    bool FAnimMontagePlayer::HasActive() const
    {
        for (const FAnimMontageInstance& Instance : Instances)
        {
            if (Instance.IsActive())
            {
                return true;
            }
        }
        return false;
    }

    FName FAnimMontagePlayer::GetCurrentSection(const CAnimationMontage* Montage) const
    {
        const FAnimMontageInstance* Instance = Find(Montage);
        if (Instance == nullptr || !Instance->Montage.IsValid())
        {
            return FName();
        }

        const int32 Index = Instance->CurrentSection;
        return (Index >= 0 && Index < (int32)Instance->Montage->Sections.size())
            ? Instance->Montage->Sections[Index].Name
            : FName();
    }

    float FAnimMontagePlayer::GetPosition(const CAnimationMontage* Montage) const
    {
        const FAnimMontageInstance* Instance = Find(Montage);
        return Instance != nullptr ? Instance->Position : 0.0f;
    }

    float FAnimMontagePlayer::GetWeight(const CAnimationMontage* Montage) const
    {
        const FAnimMontageInstance* Instance = Find(Montage);
        return Instance != nullptr ? Instance->Weight : 0.0f;
    }

    void FAnimMontagePlayer::SetPlayRate(const CAnimationMontage* Montage, float PlayRate)
    {
        if (FAnimMontageInstance* Instance = Find(Montage))
        {
            Instance->PlayRate = Math::Max(PlayRate, 0.0f);
        }
    }

    FAnimMontageInstance* FAnimMontagePlayer::Find(const CAnimationMontage* Montage)
    {
        // Replaying a montage leaves the previous instance blending out, so the live one wins.
        FAnimMontageInstance* Fallback = nullptr;

        for (FAnimMontageInstance& Instance : Instances)
        {
            if (Instance.Montage.Get() != Montage || !Instance.IsActive())
            {
                continue;
            }

            if (Instance.State != EAnimMontageState::BlendingOut)
            {
                return &Instance;
            }

            Fallback = &Instance;
        }

        return Fallback;
    }

    const FAnimMontageInstance* FAnimMontagePlayer::Find(const CAnimationMontage* Montage) const
    {
        return const_cast<FAnimMontagePlayer*>(this)->Find(Montage);
    }

    void FAnimMontagePlayer::Update(float DeltaTime, TVector<FAnimNotifyEvent>* OutEvents)
    {
        if (Instances.empty())
        {
            return;
        }

        for (FAnimMontageInstance& Instance : Instances)
        {
            if (Instance.IsActive())
            {
                AdvanceInstance(Instance, DeltaTime, OutEvents);
            }
        }

        Instances.erase(eastl::remove_if(Instances.begin(), Instances.end(), [](const FAnimMontageInstance& Instance)
        {
            return !Instance.IsActive();
        }), Instances.end());
    }

    void FAnimMontagePlayer::AdvanceInstance(FAnimMontageInstance& Instance, float DeltaTime, TVector<FAnimNotifyEvent>* OutEvents)
    {
        CAnimationMontage* Montage = Instance.Montage.Get();
        if (Montage == nullptr)
        {
            Instance.State = EAnimMontageState::Finished;
            return;
        }

        const float Target = (Instance.State == EAnimMontageState::BlendingOut) ? 0.0f : 1.0f;
        const float Step   = Instance.BlendRate * DeltaTime;

        Instance.Weight = (Instance.Weight < Target)
            ? Math::Min(Instance.Weight + Step, Target)
            : Math::Max(Instance.Weight - Step, Target);

        if (Instance.State == EAnimMontageState::BlendingOut && Instance.Weight <= 0.0f)
        {
            EndActiveNotifyStates(Instance, OutEvents);
            Instance.State = EAnimMontageState::Finished;
            return;
        }

        if (Instance.State == EAnimMontageState::BlendingIn && Instance.Weight >= 1.0f)
        {
            Instance.State = EAnimMontageState::Playing;
        }

        Instance.PrevPosition = Instance.Position;

        const float Advance = DeltaTime * Instance.PlayRate;
        Instance.Position += Advance;

        const float SectionEnd = Montage->GetSectionEndTime(Instance.CurrentSection);

        if (Instance.Position >= SectionEnd)
        {
            FName Next = Instance.QueuedSection;
            if (Next.IsNone() && Instance.CurrentSection >= 0 && Instance.CurrentSection < (int32)Montage->Sections.size())
            {
                Next = Montage->Sections[Instance.CurrentSection].NextSection;
            }

            const int32 NextIndex = Next.IsNone() ? INDEX_NONE : Montage->FindSectionIndex(Next);
            if (NextIndex != INDEX_NONE)
            {
                // Fire only up to the seam; the overshoot belongs to the section being entered.
                const float Overshoot = Instance.Position - SectionEnd;
                Instance.Position = SectionEnd;
                DispatchNotifies(Instance, Advance > 0.0f, OutEvents);

                Instance.CurrentSection = NextIndex;
                Instance.QueuedSection  = FName();
                Instance.PrevPosition   = Montage->Sections[NextIndex].StartTime;
                Instance.Position       = Montage->Sections[NextIndex].StartTime + Overshoot;
                return;
            }

            Instance.Position = SectionEnd;
            BeginBlendOut(Instance, -1.0f);
        }
        else if (Instance.State != EAnimMontageState::BlendingOut &&
                 Instance.Position >= Montage->GetBlendOutStartTime(Instance.CurrentSection))
        {
            const bool bChains = !Instance.QueuedSection.IsNone() ||
                                 (Instance.CurrentSection >= 0 &&
                                  Instance.CurrentSection < (int32)Montage->Sections.size() &&
                                  !Montage->Sections[Instance.CurrentSection].NextSection.IsNone());

            if (!bChains)
            {
                BeginBlendOut(Instance, -1.0f);
            }
        }

        DispatchNotifies(Instance, Advance > 0.0f, OutEvents);
    }

    void FAnimMontagePlayer::DispatchNotifies(FAnimMontageInstance& Instance, bool bAdvanced, TVector<FAnimNotifyEvent>* OutEvents)
    {
        CAnimationMontage* Montage = Instance.Montage.Get();
        if (OutEvents == nullptr || Montage == nullptr || Instance.Weight <= 0.01f)
        {
            return;
        }

        const float Prev = Instance.PrevPosition;
        const float Cur  = Instance.Position;

        if (bAdvanced)
        {
            for (const SAnimMontageNotify& Notify : Montage->Notifies)
            {
                if (Notify.Time > Prev && Notify.Time <= Cur)
                {
                    FAnimNotifyEvent& Event = OutEvents->emplace_back();
                    Event.Name   = Notify.Name;
                    Event.Track  = Notify.Track;
                    Event.Type   = EAnimNotifyEventType::Trigger;
                    Event.Weight = Instance.Weight;
                    Event.Notify = Notify.Notify.Get();
                }
            }
        }

        thread_local TVector<int32> NowActive;
        NowActive.clear();

        if (bAdvanced)
        {
            for (int32 i = 0; i < (int32)Montage->NotifyStates.size(); ++i)
            {
                const SAnimMontageNotifyState& State = Montage->NotifyStates[i];
                if (Cur >= State.StartTime && Cur <= State.EndTime)
                {
                    NowActive.push_back(i);
                }
            }
        }

        const auto Emit = [&](int32 Index, EAnimNotifyEventType Type)
        {
            const SAnimMontageNotifyState& State = Montage->NotifyStates[Index];
            const float Span = State.EndTime - State.StartTime;

            FAnimNotifyEvent& Event = OutEvents->emplace_back();
            Event.Name   = State.Name;
            Event.Track  = State.Track;
            Event.Type   = Type;
            Event.Weight = Instance.Weight;
            Event.State  = State.Notify.Get();
            Event.Alpha  = Span > 0.0f ? Math::Clamp((Cur - State.StartTime) / Span, 0.0f, 1.0f) : 0.0f;
        };

        for (int32 Index : NowActive)
        {
            const bool bWasActive = eastl::find(Instance.ActiveNotifyStates.begin(),
                                                Instance.ActiveNotifyStates.end(), Index) != Instance.ActiveNotifyStates.end();
            Emit(Index, bWasActive ? EAnimNotifyEventType::Tick : EAnimNotifyEventType::Begin);
        }

        for (int32 Index : Instance.ActiveNotifyStates)
        {
            if (eastl::find(NowActive.begin(), NowActive.end(), Index) == NowActive.end())
            {
                Emit(Index, EAnimNotifyEventType::End);
            }
        }

        Instance.ActiveNotifyStates.assign(NowActive.begin(), NowActive.end());
    }

    void FAnimMontagePlayer::EndActiveNotifyStates(FAnimMontageInstance& Instance, TVector<FAnimNotifyEvent>* OutEvents)
    {
        CAnimationMontage* Montage = Instance.Montage.Get();
        if (OutEvents != nullptr && Montage != nullptr)
        {
            for (int32 Index : Instance.ActiveNotifyStates)
            {
                if (Index >= 0 && Index < (int32)Montage->NotifyStates.size())
                {
                    const SAnimMontageNotifyState& State = Montage->NotifyStates[Index];
                    FAnimNotifyEvent& Event = OutEvents->emplace_back();
                    Event.Name   = State.Name;
                    Event.Track  = State.Track;
                    Event.Type   = EAnimNotifyEventType::End;
                    Event.Weight = Instance.Weight;
                    Event.State  = State.Notify.Get();
                    Event.Alpha  = 1.0f;
                }
            }
        }

        Instance.ActiveNotifyStates.clear();
    }

    void FAnimMontagePlayer::GatherSlot(const FName& SlotName, TVector<FAnimMontageSlotContribution>& OutContributions) const
    {
        OutContributions.clear();

        for (const FAnimMontageInstance& Instance : Instances)
        {
            if (!Instance.IsActive() || Instance.Weight <= 1e-3f || !Instance.Montage.IsValid())
            {
                continue;
            }

            FAnimMontageSlotSample Sample;
            if (!Instance.Montage->EvaluateSlot(SlotName, Instance.Position, Instance.PrevPosition, Sample))
            {
                continue;
            }

            FAnimMontageSlotContribution& Contribution = OutContributions.emplace_back();
            Contribution.Montage      = Instance.Montage.Get();
            Contribution.Clip         = Sample.Clip;
            Contribution.ClipTime     = Sample.ClipTime;
            Contribution.PrevClipTime = Sample.PrevClipTime;
            Contribution.bLooping     = Sample.bLooping;
            Contribution.Weight       = Math::Clamp(Instance.Weight, 0.0f, 1.0f);
            Contribution.bRootMotion  = Instance.Montage->bEnableRootMotion;
        }
    }
}
