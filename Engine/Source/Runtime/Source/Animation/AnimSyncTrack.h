#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Two clips at the same sync position are at the same point in their cycle, however their markers fall.
    struct FSyncPosition
    {
        int32 EventIndex = 0;
        float Percent    = 0.0f;

        FSyncPosition() = default;
        FSyncPosition(int32 InEventIndex, float InPercent) : EventIndex(InEventIndex), Percent(InPercent) {}

        // Packed so the VM can carry a position in one state slot; event counts stay far below float precision.
        float ToFloat() const { return (float)EventIndex + Percent; }

        static FSyncPosition FromFloat(float Value)
        {
            const float Base = Math::Floor(Value);
            return FSyncPosition((int32)Base, Value - Base);
        }
    };

    // One span of a cycle, from its own marker to the next; times are fractions of the clip.
    struct FSyncEvent
    {
        FName ID;
        float StartTime = 0.0f;

        // The last event's span wraps past the end of the clip back to the first marker.
        float Duration = 1.0f;
    };

    // A track with no authored markers holds one event, which makes a sync position a plain phase.
    class RUNTIME_API FSyncTrack
    {
    public:

        struct FMarker
        {
            FName ID;
            float StartTime = 0.0f;
        };

        FSyncTrack() { Events.push_back(FSyncEvent()); }

        // Markers must arrive sorted and free of duplicates; an empty list yields the default track.
        void BuildFromMarkers(const TVector<FMarker>& Markers);

        // Averaging start times is meaningless, so only durations blend and start times are rebuilt from them.
        void BuildBlended(const FSyncTrack& A, const FSyncTrack& B, float Alpha);

        int32 NumEvents() const { return (int32)Events.size(); }
        const FSyncEvent& GetEvent(int32 Index) const { return Events[WrapIndex(Index)]; }
        FName GetEventID(int32 Index) const { return Events[WrapIndex(Index)].ID; }

        bool IsDefault() const { return Events.size() == 1; }

        float GetPercentThrough(const FSyncPosition& Position) const;

        FSyncPosition GetPosition(float ClipPercent) const;

        // Steps by a fraction of a whole cycle, wrapping at the end of the track.
        FSyncPosition Advance(const FSyncPosition& Position, float DeltaPercent) const;

        // Each side scales up to the common event count first, so the blend compares cycles of one length.
        static float BlendDuration(float DurationA, float DurationB, int32 NumEventsA, int32 NumEventsB, float Alpha);

        // Blending to this many events is what lets a two-step cycle line up with a four-step one.
        static int32 LowestCommonMultiple(int32 A, int32 B);

    private:

        int32 WrapIndex(int32 Index) const;

        TVector<FSyncEvent, 8> Events;
    };
}
