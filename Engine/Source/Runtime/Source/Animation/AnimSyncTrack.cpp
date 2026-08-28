#include "RuntimePCH.h"
#include "AnimSyncTrack.h"

namespace Lumina
{
    namespace
    {
        constexpr float kMinEventDuration = 1e-5f;
    }

    int32 FSyncTrack::WrapIndex(int32 Index) const
    {
        const int32 Count = (int32)Events.size();
        int32 Wrapped = Index % Count;
        if (Wrapped < 0)
        {
            Wrapped += Count;
        }
        return Wrapped;
    }

    int32 FSyncTrack::LowestCommonMultiple(int32 A, int32 B)
    {
        if (A <= 0 || B <= 0)
        {
            return Math::Max(Math::Max(A, B), 1);
        }

        int32 GreatestCommonDivisor = A;
        int32 Remainder = B;
        while (Remainder != 0)
        {
            const int32 Next = GreatestCommonDivisor % Remainder;
            GreatestCommonDivisor = Remainder;
            Remainder = Next;
        }

        return (A / GreatestCommonDivisor) * B;
    }

    void FSyncTrack::BuildFromMarkers(const TVector<FMarker>& Markers)
    {
        Events.clear();

        if (Markers.empty())
        {
            Events.push_back(FSyncEvent());
            return;
        }

        Events.reserve(Markers.size());
        for (const FMarker& Marker : Markers)
        {
            FSyncEvent Event;
            Event.ID        = Marker.ID;
            Event.StartTime = Math::Saturate(Marker.StartTime);
            Events.push_back(Event);
        }

        const int32 Count = (int32)Events.size();
        for (int32 i = 0; i < Count - 1; ++i)
        {
            Events[i].Duration = Events[i + 1].StartTime - Events[i].StartTime;
        }

        Events.back().Duration = 1.0f - (Events.back().StartTime - Events[0].StartTime);

        // A degenerate span would divide by zero on every conversion, so collapse to the default track.
        for (const FSyncEvent& Event : Events)
        {
            if (Event.Duration < kMinEventDuration)
            {
                Events.clear();
                Events.push_back(FSyncEvent());
                return;
            }
        }
    }

    void FSyncTrack::BuildBlended(const FSyncTrack& A, const FSyncTrack& B, float Alpha)
    {
        const int32 CountA = A.NumEvents();
        const int32 CountB = B.NumEvents();
        const int32 Count  = LowestCommonMultiple(CountA, CountB);

        const float ScaleA = (float)CountA / (float)Count;
        const float ScaleB = (float)CountB / (float)Count;
        const float Weight = Math::Saturate(Alpha);

        // Built aside, since a chained blend passes its own destination back in as a source.
        TVector<FSyncEvent, 8> Blended;
        Blended.reserve(Count);

        float StartTime = 0.0f;
        for (int32 i = 0; i < Count; ++i)
        {
            const FSyncEvent& EventA = A.GetEvent(i);
            const FSyncEvent& EventB = B.GetEvent(i);

            FSyncEvent Event;
            Event.ID        = Weight > 0.5f ? EventB.ID : EventA.ID;
            Event.StartTime = StartTime;
            Event.Duration  = Math::Mix(EventA.Duration * ScaleA, EventB.Duration * ScaleB, Weight);

            Blended.push_back(Event);
            StartTime += Event.Duration;
        }

        if (StartTime < kMinEventDuration)
        {
            Events.clear();
            Events.push_back(FSyncEvent());
            return;
        }

        const float Normalize = 1.0f / StartTime;
        for (FSyncEvent& Event : Blended)
        {
            Event.StartTime *= Normalize;
            Event.Duration  *= Normalize;
        }

        Blended.back().Duration = 1.0f - Blended.back().StartTime;
        Events = Move(Blended);
    }

    float FSyncTrack::GetPercentThrough(const FSyncPosition& Position) const
    {
        const FSyncEvent& Event = Events[WrapIndex(Position.EventIndex)];

        // The last event runs past the end of the clip, and the end of a cycle is its own start.
        const float Percent = Event.StartTime + Event.Duration * Math::Saturate(Position.Percent);
        return Percent >= 1.0f ? Percent - 1.0f : Percent;
    }

    FSyncPosition FSyncTrack::GetPosition(float ClipPercent) const
    {
        const int32 Count = (int32)Events.size();
        const float Percent = Math::Fract(ClipPercent);

        // Anything before the first marker belongs to the last event's wrapped tail.
        if (Percent < Events[0].StartTime)
        {
            const FSyncEvent& Last = Events.back();
            const float Remaining = Events[0].StartTime - Percent;
            return FSyncPosition(Count - 1, Math::Saturate((Last.Duration - Remaining) / Last.Duration));
        }

        for (int32 i = 0; i < Count; ++i)
        {
            const FSyncEvent& Event = Events[i];
            if (Event.StartTime + Event.Duration >= Percent)
            {
                return FSyncPosition(i, Math::Saturate((Percent - Event.StartTime) / Event.Duration));
            }
        }

        return FSyncPosition(Count - 1, 1.0f);
    }

    FSyncPosition FSyncTrack::Advance(const FSyncPosition& Position, float DeltaPercent) const
    {
        // Stepped through clip time rather than event index, so a long span still takes longer.
        return GetPosition(GetPercentThrough(Position) + DeltaPercent);
    }

    float FSyncTrack::BlendDuration(float DurationA, float DurationB, int32 NumEventsA, int32 NumEventsB, float Alpha)
    {
        const int32 Count = LowestCommonMultiple(NumEventsA, NumEventsB);
        const float ScaledA = DurationA * ((float)Count / (float)Math::Max(NumEventsA, 1));
        const float ScaledB = DurationB * ((float)Count / (float)Math::Max(NumEventsB, 1));
        return Math::Mix(ScaledA, ScaledB, Math::Saturate(Alpha));
    }
}
