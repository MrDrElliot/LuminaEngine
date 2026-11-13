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

    // Per-entity ring of a SimulatedProxy's recent samples, rendered InterpDelay behind the newest server time.
    struct FNetInterpState
    {
        static constexpr int Capacity = 12;
        FNetInterpSample Samples[Capacity];
        int Count = 0; // valid samples
        int Head  = 0; // physical index of the oldest

        const FNetInterpSample& Logical(int Index) const { return Samples[(Head + Index) % Capacity]; }
        const FNetInterpSample& Newest() const { return Samples[(Head + Count - 1) % Capacity]; }

        // Effective send interval in seconds, so low-rate proxies buffer enough to interpolate instead of extrapolate.
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

        // A jump this far was a teleport, so it lands at B's time instead of sliding or extrapolating along it.
        static bool IsTeleport(const FNetInterpSample& A, const FNetInterpSample& B, float TeleportDistance)
        {
            return TeleportDistance > 0.0f && Math::Length(B.Pos - A.Pos) > TeleportDistance;
        }

        // Pose at RenderTime, held at either end, lerped between samples, or extrapolated past the newest.
        void Evaluate(double RenderTime, FVector3& OutPos, FQuat& OutRot, bool bExtrapolate, double MaxExtrapolation,
            float TeleportDistance = 0.0f) const
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
                const FNetInterpSample& Last = Logical(Count - 1);
                const FNetInterpSample& Prev = Logical(Count - 2);
                if (bExtrapolate && !IsTeleport(Prev, Last, TeleportDistance))
                {
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
                const FNetInterpSample& A = Logical(i);
                const FNetInterpSample& B = Logical(i + 1);
                if (RenderTime >= A.Time && RenderTime <= B.Time)
                {
                    if (IsTeleport(A, B, TeleportDistance))
                    {
                        OutPos = A.Pos; OutRot = A.Rot; return;
                    }
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

    // Bumped each time the quantized pose or scale changes, so each client can compare against what it was last sent.
    struct FNetChangeSeq
    {
        uint32 Pose  = 0;
        uint32 Scale = 0;
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

        // A character's view pitch as last sent, in the same steps the wire carries.
        int8  LastSentPitch     = 0;

        FNetChangeSeq ChangeSeq;

        //~ Receive side. Timestamped sample ring for interpolation/extrapolation.
        FNetInterpState Ring;

        // Smoothed per-entity interpolation delay (seconds). Eased toward max(InterpDelay, BufferIntervals *
        // Ring.AverageInterval()) so the render clock stays behind this entity's actual sample rate (no routine
        // extrapolation/snap) and a tier/rate change ramps the delay instead of rewinding. -1 = uninitialized.
        double SmoothedInterpDelay = -1.0;

        // Per second, so a 60 fps tick eases the delay by the same 10% the old per-tick factor did.
        static constexpr float InterpDelayEaseRate = 6.32f;

        // The first sample snaps to TargetDelay so a new proxy does not ramp up from zero.
        double EasedInterpDelay(double TargetDelay, double DeltaTime) const
        {
            if (SmoothedInterpDelay < 0.0)
            {
                return TargetDelay;
            }
            return Math::Damp(static_cast<float>(SmoothedInterpDelay), static_cast<float>(TargetDelay), InterpDelayEaseRate,
                static_cast<float>(DeltaTime));
        }

        // Latest received scale (not interpolated; applied directly). Valid once bHasScale is set.
        NetQuantize::FQuantizedVector CurrentScaleQ;
        bool bHasScale = false;
    };
}
