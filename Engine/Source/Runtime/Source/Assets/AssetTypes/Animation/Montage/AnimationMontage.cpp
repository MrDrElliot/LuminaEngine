#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AnimationMontage.h"

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    float SAnimMontageSegment::GetTrimmedLength() const
    {
        if (!Animation.IsValid())
        {
            return 0.0f;
        }

        const float ClipDuration = Animation->GetDuration();
        const float Start = Math::Clamp(ClipStartTime, 0.0f, ClipDuration);
        const float End = (ClipEndTime > 0.0f) ? Math::Min(ClipEndTime, ClipDuration) : ClipDuration;

        return Math::Max(End - Start, 0.0f);
    }

    float SAnimMontageSegment::GetTimelineLength() const
    {
        const float Rate = Math::Max(PlayRate, 0.01f);
        const int32 Loops = Math::Max(LoopCount, 1);

        return (GetTrimmedLength() * (float)Loops) / Rate;
    }

    float SAnimMontageSegment::ResolveClipTime(float MontageTime) const
    {
        const float Trimmed = GetTrimmedLength();
        if (Trimmed <= 0.0f)
        {
            return ClipStartTime;
        }

        const float Rate = Math::Max(PlayRate, 0.01f);
        const float Local = Math::Max(MontageTime - StartTime, 0.0f) * Rate;

        // fmod keeps repeats inside the trim window; the last frame clamps instead of wrapping to zero.
        float Wrapped = fmodf(Local, Trimmed);
        if (Local > 0.0f && Wrapped == 0.0f)
        {
            Wrapped = Trimmed;
        }

        return ClipStartTime + Math::Min(Wrapped, Trimmed);
    }

    float SAnimMontageSlotTrack::GetEndTime() const
    {
        float End = 0.0f;
        for (const SAnimMontageSegment& Segment : Segments)
        {
            End = Math::Max(End, Segment.GetEndTime());
        }
        return End;
    }

    int32 SAnimMontageSlotTrack::FindSegment(float MontageTime) const
    {
        for (int32 i = 0; i < (int32)Segments.size(); ++i)
        {
            const SAnimMontageSegment& Segment = Segments[i];
            if (MontageTime >= Segment.StartTime && MontageTime < Segment.GetEndTime())
            {
                return i;
            }
        }

        // The final frame sits exactly on the end boundary, so hold the last segment there.
        int32 Last = INDEX_NONE;
        float LastEnd = 0.0f;
        for (int32 i = 0; i < (int32)Segments.size(); ++i)
        {
            const float End = Segments[i].GetEndTime();
            if (End > LastEnd)
            {
                LastEnd = End;
                Last = i;
            }
        }

        return (Last != INDEX_NONE && Math::Abs(MontageTime - LastEnd) < 1e-4f) ? Last : INDEX_NONE;
    }

    void CAnimationMontage::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Animation");
        Super::PostLoad();
        EnsureNotifyTracks();
    }

    float CAnimationMontage::GetDuration() const
    {
        float Duration = 0.0f;
        for (const SAnimMontageSlotTrack& Track : SlotTracks)
        {
            Duration = Math::Max(Duration, Track.GetEndTime());
        }
        return Duration;
    }

    int32 CAnimationMontage::FindSlotIndex(const FName& SlotName) const
    {
        for (int32 i = 0; i < (int32)SlotTracks.size(); ++i)
        {
            if (SlotTracks[i].SlotName == SlotName)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    int32 CAnimationMontage::FindSectionIndex(const FName& SectionName) const
    {
        for (int32 i = 0; i < (int32)Sections.size(); ++i)
        {
            if (Sections[i].Name == SectionName)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    int32 CAnimationMontage::FindSectionAtTime(float MontageTime) const
    {
        int32 Best = INDEX_NONE;
        float BestStart = -1.0f;

        for (int32 i = 0; i < (int32)Sections.size(); ++i)
        {
            const float Start = Sections[i].StartTime;
            if (Start <= MontageTime && Start >= BestStart)
            {
                BestStart = Start;
                Best = i;
            }
        }

        return Best;
    }

    float CAnimationMontage::GetSectionEndTime(int32 SectionIndex) const
    {
        const float Duration = GetDuration();
        if (SectionIndex < 0 || SectionIndex >= (int32)Sections.size())
        {
            return Duration;
        }

        const float Start = Sections[SectionIndex].StartTime;
        float End = Duration;

        for (const SAnimMontageSection& Other : Sections)
        {
            if (Other.StartTime > Start && Other.StartTime < End)
            {
                End = Other.StartTime;
            }
        }

        return End;
    }

    float CAnimationMontage::GetBlendOutStartTime(int32 SectionIndex) const
    {
        const float End = GetSectionEndTime(SectionIndex);
        const float Lead = (BlendOutTriggerTime >= 0.0f) ? BlendOutTriggerTime : BlendOutTime;

        return Math::Max(End - Lead, 0.0f);
    }

    bool CAnimationMontage::EvaluateSlot(const FName& SlotName, float MontageTime, float PrevMontageTime,
                                         FAnimMontageSlotSample& OutSample) const
    {
        const int32 SlotIndex = FindSlotIndex(SlotName);
        if (SlotIndex == INDEX_NONE)
        {
            return false;
        }

        const SAnimMontageSlotTrack& Track = SlotTracks[SlotIndex];
        const int32 SegmentIndex = Track.FindSegment(MontageTime);
        if (SegmentIndex == INDEX_NONE)
        {
            return false;
        }

        const SAnimMontageSegment& Segment = Track.Segments[SegmentIndex];
        if (!Segment.Animation.IsValid())
        {
            return false;
        }

        OutSample.Clip = Segment.Animation.Get();
        OutSample.ClipTime = Segment.ResolveClipTime(MontageTime);
        OutSample.bLooping = Segment.LoopCount > 1;

        // A delta spanning a segment boundary is meaningless, so collapse the window instead.
        const bool bPrevInSegment = PrevMontageTime >= Segment.StartTime && PrevMontageTime <= Segment.GetEndTime();
        OutSample.PrevClipTime = bPrevInSegment ? Segment.ResolveClipTime(PrevMontageTime) : OutSample.ClipTime;

        return true;
    }

    void CAnimationMontage::GetSlotNames(TVector<FName>& OutNames) const
    {
        for (const SAnimMontageSlotTrack& Track : SlotTracks)
        {
            if (!Track.SlotName.IsNone() &&
                std::find(OutNames.begin(), OutNames.end(), Track.SlotName) == OutNames.end())
            {
                OutNames.push_back(Track.SlotName);
            }
        }
    }

    void CAnimationMontage::EnsureNotifyTracks()
    {
        const auto AddTrack = [this](const FName& Track)
        {
            if (!Track.IsNone() && std::find(NotifyTracks.begin(), NotifyTracks.end(), Track) == NotifyTracks.end())
            {
                NotifyTracks.push_back(Track);
            }
        };

        for (const SAnimMontageNotify& Notify : Notifies)
        {
            AddTrack(Notify.Track);
        }
        for (const SAnimMontageNotifyState& State : NotifyStates)
        {
            AddTrack(State.Track);
        }

        if (NotifyTracks.empty())
        {
            NotifyTracks.push_back("Notifies");
        }
    }
}
