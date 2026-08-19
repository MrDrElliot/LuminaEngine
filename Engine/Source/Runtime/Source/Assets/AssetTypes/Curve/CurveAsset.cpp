#include "RuntimePCH.h"
#include "CurveAsset.h"

#include "Core/Math/Math.h"

namespace Lumina
{
    namespace
    {
        constexpr float CurveMinSegment = 1e-6f;

        float SegmentSlope(const SCurveKey& A, const SCurveKey& B)
        {
            const float Dt = B.Time - A.Time;
            return Dt > CurveMinSegment ? (B.Value - A.Value) / Dt : 0.0f;
        }
    }

    float SKeyedCurve::EvaluateInRange(float InTime) const
    {
        if (Keys.empty())
        {
            return 0.0f;
        }

        const int32 Last = (int32)Keys.size() - 1;
        if (Keys.size() == 1 || InTime <= Keys[0].Time)
        {
            return Keys[0].Value;
        }
        if (InTime >= Keys[Last].Time)
        {
            return Keys[Last].Value;
        }

        // Last key whose time is <= InTime.
        int32 Lo = 0;
        int32 Hi = Last;
        while (Lo < Hi)
        {
            const int32 Mid = (Lo + Hi + 1) / 2;
            if (Keys[Mid].Time <= InTime)
            {
                Lo = Mid;
            }
            else
            {
                Hi = Mid - 1;
            }
        }

        const SCurveKey& A = Keys[Lo];
        const SCurveKey& B = Keys[Lo + 1];
        const float Dt = B.Time - A.Time;
        if (Dt <= CurveMinSegment)
        {
            return B.Value;
        }

        const float Alpha = (InTime - A.Time) / Dt;

        switch (A.InterpMode)
        {
        case ECurveInterpMode::Constant:
            return A.Value;

        case ECurveInterpMode::Cubic:
        case ECurveInterpMode::CubicUser:
            {
                // Hermite basis; slopes are scaled into segment space by Dt.
                const float M0 = A.LeaveTangent * Dt;
                const float M1 = B.ArriveTangent * Dt;
                const float T2 = Alpha * Alpha;
                const float T3 = T2 * Alpha;

                return (2.0f * T3 - 3.0f * T2 + 1.0f) * A.Value
                     + (T3 - 2.0f * T2 + Alpha) * M0
                     + (-2.0f * T3 + 3.0f * T2) * B.Value
                     + (T3 - T2) * M1;
            }

        case ECurveInterpMode::Linear:
        default:
            return Math::Lerp(A.Value, B.Value, Alpha);
        }
    }

    float SKeyedCurve::Extrapolate(float InTime, ECurveExtrapolation Mode, bool bBefore) const
    {
        const SCurveKey& First = Keys.front();
        const SCurveKey& Last = Keys.back();
        const float Span = Last.Time - First.Time;

        if (Span <= CurveMinSegment)
        {
            return bBefore ? First.Value : Last.Value;
        }

        switch (Mode)
        {
        case ECurveExtrapolation::Linear:
            {
                if (bBefore)
                {
                    const float Slope = First.IsCubic() ? First.LeaveTangent : SegmentSlope(First, Keys[1]);
                    return First.Value + Slope * (InTime - First.Time);
                }

                const SCurveKey& Prev = Keys[Keys.size() - 2];
                const float Slope = Last.IsCubic() ? Last.ArriveTangent : SegmentSlope(Prev, Last);
                return Last.Value + Slope * (InTime - Last.Time);
            }

        case ECurveExtrapolation::Cycle:
            {
                const float Delta = InTime - First.Time;
                const float Cycles = Math::Floor(Delta / Span);
                return EvaluateInRange(First.Time + (Delta - Cycles * Span));
            }

        case ECurveExtrapolation::CycleWithOffset:
            {
                const float Delta = InTime - First.Time;
                const float Cycles = Math::Floor(Delta / Span);
                return EvaluateInRange(First.Time + (Delta - Cycles * Span)) + Cycles * (Last.Value - First.Value);
            }

        case ECurveExtrapolation::Oscillate:
            {
                const float Delta = InTime - First.Time;
                const float Cycles = Math::Floor(Delta / Span);
                float Local = Delta - Cycles * Span;
                if (((int64)Cycles & 1) != 0)
                {
                    Local = Span - Local;
                }
                return EvaluateInRange(First.Time + Local);
            }

        case ECurveExtrapolation::Clamp:
        default:
            return bBefore ? First.Value : Last.Value;
        }
    }

    float SKeyedCurve::Evaluate(float InTime) const
    {
        if (Keys.empty())
        {
            return 0.0f;
        }
        if (Keys.size() == 1)
        {
            return Keys[0].Value;
        }

        if (InTime < Keys.front().Time)
        {
            return Extrapolate(InTime, PreExtrapolation, true);
        }
        if (InTime > Keys.back().Time)
        {
            return Extrapolate(InTime, PostExtrapolation, false);
        }

        return EvaluateInRange(InTime);
    }

    void SKeyedCurve::BakeSegments(TVector<FCurveSegment>& OutSegments) const
    {
        OutSegments.clear();
        if (Keys.size() < 2)
        {
            return;
        }

        OutSegments.reserve(Keys.size() - 1);

        for (size_t Index = 0; Index + 1 < Keys.size(); ++Index)
        {
            const SCurveKey& A = Keys[Index];
            const SCurveKey& B = Keys[Index + 1];

            FCurveSegment Segment;
            Segment.StartTime = A.Time;
            Segment.Duration = B.Time - A.Time;

            if (Segment.Duration <= CurveMinSegment)
            {
                // EvaluateInRange collapses a zero-width segment to the trailing key.
                Segment.Duration = 0.0f;
                Segment.A = B.Value;
                OutSegments.push_back(Segment);
                continue;
            }

            switch (A.InterpMode)
            {
            case ECurveInterpMode::Constant:
                Segment.A = A.Value;
                break;

            case ECurveInterpMode::Cubic:
            case ECurveInterpMode::CubicUser:
                {
                    // Same Hermite basis as EvaluateInRange, expanded into polynomial coefficients.
                    const float M0 = A.LeaveTangent * Segment.Duration;
                    const float M1 = B.ArriveTangent * Segment.Duration;

                    Segment.A = A.Value;
                    Segment.B = M0;
                    Segment.C = -3.0f * A.Value - 2.0f * M0 + 3.0f * B.Value - M1;
                    Segment.D = 2.0f * A.Value + M0 - 2.0f * B.Value + M1;
                }
                break;

            case ECurveInterpMode::Linear:
            default:
                Segment.A = A.Value;
                Segment.B = B.Value - A.Value;
                break;
            }

            OutSegments.push_back(Segment);
        }
    }

    void SKeyedCurve::GetExtrapolationSlopes(float& OutPreSlope, float& OutPostSlope) const
    {
        OutPreSlope = 0.0f;
        OutPostSlope = 0.0f;

        if (Keys.size() < 2)
        {
            return;
        }

        const SCurveKey& First = Keys.front();
        const SCurveKey& Last = Keys.back();
        const SCurveKey& Prev = Keys[Keys.size() - 2];

        OutPreSlope = First.IsCubic() ? First.LeaveTangent : SegmentSlope(First, Keys[1]);
        OutPostSlope = Last.IsCubic() ? Last.ArriveTangent : SegmentSlope(Prev, Last);
    }

    int32 SKeyedCurve::AddKey(float InTime, float InValue)
    {
        int32 Index = 0;
        while (Index < (int32)Keys.size() && Keys[Index].Time <= InTime)
        {
            ++Index;
        }

        SCurveKey NewKey;
        NewKey.Time = InTime;
        NewKey.Value = InValue;

        // Inherit the shape of the segment being split. User tangents don't transfer, so those
        // fall back to an auto key that ComputeAutoTangents will fill in.
        if (!Keys.empty())
        {
            const ECurveInterpMode Neighbor = Keys[Math::Max(Index - 1, 0)].InterpMode;
            NewKey.InterpMode = Neighbor == ECurveInterpMode::CubicUser ? ECurveInterpMode::Cubic : Neighbor;
        }

        Keys.insert(Keys.begin() + Index, NewKey);
        return Index;
    }

    int32 SKeyedCurve::UpdateOrAddKey(float InTime, float InValue, float Tolerance)
    {
        for (int32 Index = 0; Index < (int32)Keys.size(); ++Index)
        {
            if (Math::Abs(Keys[Index].Time - InTime) <= Tolerance)
            {
                // Value only: the key's authored interpolation and tangents are the user's, not something
                // to reset every time they re-pose the frame.
                Keys[Index].Value = InValue;

                // Collapse any duplicates already stacked here. Curves authored before AddKey stopped being
                // called blind still carry them, and each one is a jump the user cannot see or select.
                int32 Next = Index + 1;
                while (Next < (int32)Keys.size() && Math::Abs(Keys[Next].Time - InTime) <= Tolerance)
                {
                    ++Next;
                }

                if (Next > Index + 1)
                {
                    Keys.erase(Keys.begin() + (Index + 1), Keys.begin() + Next);
                }

                return Index;
            }
        }

        return AddKey(InTime, InValue);
    }

    void SKeyedCurve::RemoveKey(int32 Index)
    {
        if (Index >= 0 && Index < (int32)Keys.size())
        {
            Keys.erase(Keys.begin() + Index);
        }
    }

    void SKeyedCurve::SortKeys()
    {
        std::stable_sort(Keys.begin(), Keys.end(), [](const SCurveKey& A, const SCurveKey& B)
        {
            return A.Time < B.Time;
        });
    }

    void SKeyedCurve::ComputeAutoTangents()
    {
        const int32 Num = (int32)Keys.size();
        if (Num == 0)
        {
            return;
        }

        for (int32 Index = 0; Index < Num; ++Index)
        {
            SCurveKey& Key = Keys[Index];
            if (Key.InterpMode != ECurveInterpMode::Cubic)
            {
                continue;
            }

            float Tangent = 0.0f;
            if (Num > 1)
            {
                if (Index == 0)
                {
                    Tangent = SegmentSlope(Key, Keys[1]);
                }
                else if (Index == Num - 1)
                {
                    Tangent = SegmentSlope(Keys[Index - 1], Key);
                }
                else
                {
                    Tangent = SegmentSlope(Keys[Index - 1], Keys[Index + 1]);
                }
            }

            Key.ArriveTangent = Tangent;
            Key.LeaveTangent = Tangent;
            Key.bTangentsBroken = false;
        }
    }

    void SKeyedCurve::GetTimeRange(float& OutMin, float& OutMax) const
    {
        if (Keys.empty())
        {
            OutMin = 0.0f;
            OutMax = 1.0f;
            return;
        }

        OutMin = Keys.front().Time;
        OutMax = Keys.back().Time;
    }

    void SKeyedCurve::GetValueRange(float& OutMin, float& OutMax) const
    {
        if (Keys.empty())
        {
            OutMin = 0.0f;
            OutMax = 1.0f;
            return;
        }

        OutMin = Keys[0].Value;
        OutMax = Keys[0].Value;
        for (const SCurveKey& Key : Keys)
        {
            OutMin = Math::Min(OutMin, Key.Value);
            OutMax = Math::Max(OutMax, Key.Value);
        }
    }

    CCurveAsset::CCurveAsset()
    {
        Curve.Keys.reserve(2);
        Curve.AddKey(0.0f, 0.0f);
        Curve.AddKey(1.0f, 1.0f);
    }

    bool SCurve::IsUsingAsset() const
    {
        return bUseAsset && Asset.Get() != nullptr;
    }

    const SKeyedCurve& SCurve::Resolve() const
    {
        // Falling back to the inline curve rather than returning null keeps every consumer branch-free:
        // an unset or destroyed asset reference degrades to whatever was authored inline instead of
        // needing a null check at each sample site.
        if (const CCurveAsset* Resolved = Asset.Get(); bUseAsset && Resolved != nullptr)
        {
            return Resolved->Curve;
        }
        return Curve;
    }
}
