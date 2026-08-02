#pragma once

#include "Core/Math/Math.h"
#include "Core/Serialization/NetQuantize.h"

namespace Lumina
{
    // One timestamped transform sample for client snapshot interpolation. Time is server time.
    struct FNetInterpSample
    {
        double   Time = 0.0;
        FVector3 Pos;
        FQuat    Rot;
    };

    // Per-entity ring of recent samples for a SimulatedProxy. The client renders InterpDelay behind the
    // newest server time, lerping between bracketing samples and (optionally) extrapolating past the newest.
    struct FNetInterpState
    {
        static constexpr int Capacity = 12;
        FNetInterpSample Samples[Capacity];
        int Count = 0; // valid samples
        int Head  = 0; // physical index of the oldest

        FNetInterpSample& Logical(int Index) { return Samples[(Head + Index) % Capacity]; }
        const FNetInterpSample& Newest() const { return Samples[(Head + Count - 1) % Capacity]; }

        // Average spacing between buffered samples (seconds), i.e. this entity's effective send interval.
        // 0 when there aren't yet two samples. Drives the adaptive interp delay so low-rate (LOD-far) proxies
        // buffer enough to interpolate instead of extrapolate.
        double AverageInterval() const
        {
            if (Count < 2)
            {
                return 0.0;
            }
            const double Oldest = Samples[Head % Capacity].Time;
            const double NewestT = Samples[(Head + Count - 1) % Capacity].Time;
            return (NewestT - Oldest) / static_cast<double>(Count - 1);
        }

        void Push(double Time, const FVector3& Pos, const FQuat& Rot)
        {
            if (Count < Capacity)
            {
                Samples[(Head + Count) % Capacity] = { Time, Pos, Rot };
                ++Count;
            }
            else
            {
                Samples[Head] = { Time, Pos, Rot }; // overwrite oldest
                Head = (Head + 1) % Capacity;
            }
        }

        // Pose at RenderTime. Holds at the low end, lerps between bracketing samples, and past the newest
        // sample extrapolates position from the last velocity (capped) when bExtrapolate; rotation is held.
        void Evaluate(double RenderTime, FVector3& OutPos, FQuat& OutRot, bool bExtrapolate, double MaxExtrapolation)
        {
            if (Count == 0)
            {
                return;
            }
            if (Count == 1 || RenderTime <= Logical(0).Time)
            {
                OutPos = Logical(0).Pos; OutRot = Logical(0).Rot; return;
            }
            if (RenderTime >= Logical(Count - 1).Time)
            {
                FNetInterpSample& Last = Logical(Count - 1);
                if (bExtrapolate)
                {
                    FNetInterpSample& Prev = Logical(Count - 2);
                    const double Span = Last.Time - Prev.Time;
                    if (Span > 1e-9)
                    {
                        double Dt = RenderTime - Last.Time;
                        if (Dt > MaxExtrapolation) { Dt = MaxExtrapolation; }
                        const float Scale = static_cast<float>(Dt / Span);
                        OutPos = Last.Pos + (Last.Pos - Prev.Pos) * Scale;
                        OutRot = Last.Rot; // angular extrapolation overshoots/jitters; hold instead
                        return;
                    }
                }
                OutPos = Last.Pos; OutRot = Last.Rot; return;
            }
            for (int i = 0; i < Count - 1; ++i)
            {
                FNetInterpSample& A = Logical(i);
                FNetInterpSample& B = Logical(i + 1);
                if (RenderTime >= A.Time && RenderTime <= B.Time)
                {
                    const double Span = B.Time - A.Time;
                    const float  T    = (Span > 1e-9) ? static_cast<float>((RenderTime - A.Time) / Span) : 0.0f;
                    OutPos = Math::Mix(A.Pos, B.Pos, T);
                    OutRot = Math::Slerp(A.Rot, B.Rot, T);
                    return;
                }
            }
            OutPos = Logical(Count - 1).Pos; OutRot = Logical(Count - 1).Rot;
        }
    };

    // Transient, networking-only replicated transform.
    struct FRepTransform
    {
        // Mirror of SNetworkComponent::NetGUID, set on add. Lets the parallel interp pass avoid GuidTable.
        uint32 NetGUID = 0;

        //~ Send side. Quantized last-sent pose; integer compare drives change-detection, send re-uses it
        //  verbatim (zero re-quantize).
        NetQuantize::FQuantizedVector LastSentPos;
        NetQuantize::FQuantizedQuat   LastSentRot;
        NetQuantize::FQuantizedVector LastSentScale;
        bool  bSendCacheValid   = false;
        float TimeSinceLastSend = 0.0f;

        //~ Receive side. Timestamped sample ring for interpolation/extrapolation.
        FNetInterpState Ring;

        // Smoothed per-entity interpolation delay (seconds). Eased toward max(InterpDelay, BufferIntervals *
        // Ring.AverageInterval()) so the render clock stays behind this entity's actual sample rate (no routine
        // extrapolation/snap) and a tier/rate change ramps the delay instead of rewinding. -1 = uninitialized.
        double SmoothedInterpDelay = -1.0;

        // Latest received scale (not interpolated; applied directly). Valid once bHasScale is set.
        NetQuantize::FQuantizedVector CurrentScaleQ;
        bool bHasScale = false;
    };
}
