#include "RuntimePCH.h"
#include "SplineComponent.h"

namespace Lumina
{
    namespace
    {
        // T0 is the LEAVE tangent and T1 the ARRIVE tangent, which is what lets a point have a corner.
        FVector3 HermitePosition(const FVector3& P0, const FVector3& T0, const FVector3& P1, const FVector3& T1, float t)
        {
            const float t2 = t * t;
            const float t3 = t2 * t;

            return P0 * (2.0f * t3 - 3.0f * t2 + 1.0f)
                 + T0 * (t3 - 2.0f * t2 + t)
                 + P1 * (-2.0f * t3 + 3.0f * t2)
                 + T1 * (t3 - t2);
        }

        FVector3 HermiteDerivative(const FVector3& P0, const FVector3& T0, const FVector3& P1, const FVector3& T1, float t)
        {
            const float t2 = t * t;

            return P0 * (6.0f * t2 - 6.0f * t)
                 + T0 * (3.0f * t2 - 4.0f * t + 1.0f)
                 + P1 * (-6.0f * t2 + 6.0f * t)
                 + T1 * (3.0f * t2 - 2.0f * t);
        }

        // Math::Lerp is ambiguous for vectors, since both the generic and vector overloads match exactly.
        FVector3 LerpV(const FVector3& A, const FVector3& B, float Alpha)
        {
            return A + (B - A) * Alpha;
        }

        // Rotate V about the (normalized) axis A by Radians. Rodrigues.
        FVector3 RotateAboutAxis(const FVector3& V, const FVector3& A, float Radians)
        {
            const float C = Math::Cos(Radians);
            const float S = Math::Sin(Radians);
            return V * C + Math::Cross(A, V) * S + A * (Math::Dot(A, V) * (1.0f - C));
        }
    }

    int32 SSplineComponent::GetNumSegments() const
    {
        const int32 NumPoints = static_cast<int32>(Points.size());
        if (NumPoints < 2)
        {
            return 0;
        }

        return bClosedLoop ? NumPoints : NumPoints - 1;
    }

    void SSplineComponent::UpdateTangents()
    {
        const int32 NumPoints = static_cast<int32>(Points.size());
        if (NumPoints < 2)
        {
            // A lone point has no direction to derive; leave whatever was authored.
            return;
        }

        for (int32 i = 0; i < NumPoints; ++i)
        {
            SSplinePoint& Point = Points[i];
            if (Point.TangentMode == ESplineTangentMode::User)
            {
                continue;
            }

            // Open-spline ends fall back to the one segment they touch, so the curve cannot flick out.
            const bool bHasPrev = bClosedLoop || (i > 0);
            const bool bHasNext = bClosedLoop || (i < NumPoints - 1);

            const FVector3& Prev = Points[(i - 1 + NumPoints) % NumPoints].Location;
            const FVector3& Next = Points[(i + 1) % NumPoints].Location;
            const FVector3& Curr = Point.Location;

            if (Point.TangentMode == ESplineTangentMode::Linear)
            {
                // Matching the segment's own chord on both endpoints collapses the Hermite to a straight line.
                Point.ArriveTangent = bHasPrev ? (Curr - Prev) : (Next - Curr);
                Point.LeaveTangent  = bHasNext ? (Next - Curr) : (Curr - Prev);
            }
            else
            {
                // Catmull-Rom.
                FVector3 Tangent;
                if (bHasPrev && bHasNext)
                {
                    Tangent = (Next - Prev) * 0.5f;
                }
                else if (bHasNext)
                {
                    Tangent = Next - Curr;
                }
                else
                {
                    Tangent = Curr - Prev;
                }

                Point.ArriveTangent = Tangent;
                Point.LeaveTangent  = Tangent;
            }
        }
    }

    FVector3 SSplineComponent::EvaluatePosition(float Key) const
    {
        const int32 NumSegments = GetNumSegments();
        if (NumSegments == 0)
        {
            return Points.empty() ? FVector3(0.0f) : Points[0].Location;
        }

        const int32 Segment = Math::Clamp(static_cast<int32>(Math::Floor(Key)), 0, NumSegments - 1);
        const float t       = Math::Clamp(Key - static_cast<float>(Segment), 0.0f, 1.0f);

        const SSplinePoint& A = Points[Segment];
        const SSplinePoint& B = Points[(Segment + 1) % static_cast<int32>(Points.size())];

        return HermitePosition(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, t);
    }

    FVector3 SSplineComponent::EvaluateTangent(float Key) const
    {
        const int32 NumSegments = GetNumSegments();
        if (NumSegments == 0)
        {
            return FVector3(0.0f, 0.0f, 1.0f);
        }

        const int32 Segment = Math::Clamp(static_cast<int32>(Math::Floor(Key)), 0, NumSegments - 1);
        const float t       = Math::Clamp(Key - static_cast<float>(Segment), 0.0f, 1.0f);

        const SSplinePoint& A = Points[Segment];
        const SSplinePoint& B = Points[(Segment + 1) % static_cast<int32>(Points.size())];

        return HermiteDerivative(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, t);
    }

    FVector3 SSplineComponent::EvaluateScale(float Key) const
    {
        const int32 NumSegments = GetNumSegments();
        if (NumSegments == 0)
        {
            return Points.empty() ? FVector3(1.0f) : Points[0].Scale;
        }

        const int32 Segment = Math::Clamp(static_cast<int32>(Math::Floor(Key)), 0, NumSegments - 1);
        const float t       = Math::Clamp(Key - static_cast<float>(Segment), 0.0f, 1.0f);

        const SSplinePoint& A = Points[Segment];
        const SSplinePoint& B = Points[(Segment + 1) % static_cast<int32>(Points.size())];

        return LerpV(A.Scale, B.Scale, t);
    }

    float SSplineComponent::EvaluateRoll(float Key) const
    {
        const int32 NumSegments = GetNumSegments();
        if (NumSegments == 0)
        {
            return Points.empty() ? 0.0f : Points[0].Roll;
        }

        const int32 Segment = Math::Clamp(static_cast<int32>(Math::Floor(Key)), 0, NumSegments - 1);
        const float t       = Math::Clamp(Key - static_cast<float>(Segment), 0.0f, 1.0f);

        const SSplinePoint& A = Points[Segment];
        const SSplinePoint& B = Points[(Segment + 1) % static_cast<int32>(Points.size())];

        return A.Roll + (B.Roll - A.Roll) * t;
    }

    FVector3 SSplineComponent::EvaluateUpVector(float Key) const
    {
        const FVector3 Derivative = EvaluateTangent(Key);
        const float    DerivLenSq = Math::LengthSquared(Derivative);

        // A zero derivative at a cusp has no frame to build, so hand back the reference up.
        if (DerivLenSq <= 1.0e-12f)
        {
            return Math::Normalize(DefaultUpVector);
        }

        const FVector3 Tangent = Derivative * (1.0f / Math::Sqrt(DerivLenSq));

        FVector3 Reference = DefaultUpVector;
        if (Math::LengthSquared(Reference) <= 1.0e-12f)
        {
            Reference = FVector3(0.0f, 1.0f, 0.0f);
        }
        Reference = Math::Normalize(Reference);

        // Gram-Schmidt against the tangent, falling back to another axis when the projection vanishes.
        FVector3 Up = Reference - Tangent * Math::Dot(Reference, Tangent);
        if (Math::LengthSquared(Up) <= 1.0e-6f)
        {
            const FVector3 Fallback = Math::Abs(Tangent.y) < 0.9f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            Up = Fallback - Tangent * Math::Dot(Fallback, Tangent);
        }
        Up = Math::Normalize(Up);

        const float Roll = EvaluateRoll(Key);
        if (Math::Abs(Roll) > 1.0e-4f)
        {
            Up = RotateAboutAxis(Up, Tangent, Math::Radians(Roll));
        }

        return Up;
    }

    float BuildSplineSamples(const SSplineComponent& Spline, const FMatrix4& LocalToWorld, TVector<FSplineSample>& OutSamples)
    {
        OutSamples.clear();

        const int32 NumSegments = Spline.GetNumSegments();
        if (NumSegments == 0)
        {
            // A single point still has a defined zero-length sample, so shaders need no special case.
            if (!Spline.Points.empty())
            {
                FSplineSample Only;
                Only.Position = FVector3(LocalToWorld * FVector4(Spline.Points[0].Location, 1.0f));
                Only.Tangent  = FVector3(0.0f, 0.0f, 1.0f);
                Only.Up       = Math::Normalize(Spline.DefaultUpVector);
                Only.Roll     = Spline.Points[0].Roll;
                Only.Scale    = Spline.Points[0].Scale;
                OutSamples.push_back(Only);
            }
            return 0.0f;
        }

        const int32 PerSegment = Math::Clamp(Spline.SamplesPerSegment, 2, 256);

        // Pass 1 accumulates chord length, oversampled so the estimate avoids the output table's error.
        const int32 DenseCount = NumSegments * PerSegment * 4 + 1;
        const float KeyRange   = static_cast<float>(NumSegments);

        TVector<FSplineSample> Dense;
        Dense.reserve(DenseCount);

        float RunningLength = 0.0f;
        for (int32 i = 0; i < DenseCount; ++i)
        {
            const float Key = (static_cast<float>(i) / static_cast<float>(DenseCount - 1)) * KeyRange;

            FSplineSample Sample;
            Sample.Key      = Key;
            Sample.Position = FVector3(LocalToWorld * FVector4(Spline.EvaluatePosition(Key), 1.0f));
            // Direction only, so w is 0 and the translation does not come along.
            Sample.Tangent  = Math::Normalize(FVector3(LocalToWorld * FVector4(Spline.EvaluateTangent(Key), 0.0f)));
            Sample.Up       = Math::Normalize(FVector3(LocalToWorld * FVector4(Spline.EvaluateUpVector(Key), 0.0f)));
            Sample.Roll     = Spline.EvaluateRoll(Key);
            Sample.Scale    = Spline.EvaluateScale(Key);

            if (i > 0)
            {
                RunningLength += Math::Distance(Dense.back().Position, Sample.Position);
            }
            Sample.DistanceAlong = RunningLength;

            Dense.push_back(Sample);
        }

        const float TotalLength = RunningLength;

        // Pass 2 resamples at a uniform distance step, so a shader turns distance into an index.
        const int32 OutCount = NumSegments * PerSegment + 1;
        OutSamples.reserve(OutCount);

        if (TotalLength <= 1.0e-6f)
        {
            // Degenerate spline still emits the full table so the GPU sample count matches the header.
            for (int32 i = 0; i < OutCount; ++i)
            {
                OutSamples.push_back(Dense.front());
            }
            return 0.0f;
        }

        const float Step = TotalLength / static_cast<float>(OutCount - 1);

        int32 Cursor = 0;
        for (int32 i = 0; i < OutCount; ++i)
        {
            const float Target = Math::Min(static_cast<float>(i) * Step, TotalLength);

            // Dense distances are monotonic, so the cursor only moves forward and the resample is O(n).
            while (Cursor + 2 < static_cast<int32>(Dense.size()) && Dense[Cursor + 1].DistanceAlong < Target)
            {
                ++Cursor;
            }

            const FSplineSample& A = Dense[Cursor];
            const FSplineSample& B = Dense[Cursor + 1];

            const float Span  = B.DistanceAlong - A.DistanceAlong;
            const float Alpha = Span > 1.0e-8f ? Math::Clamp((Target - A.DistanceAlong) / Span, 0.0f, 1.0f) : 0.0f;

            FSplineSample Sample;
            Sample.Position      = LerpV(A.Position, B.Position, Alpha);
            Sample.DistanceAlong = Target;
            Sample.Tangent       = Math::Normalize(LerpV(A.Tangent, B.Tangent, Alpha));
            Sample.Key           = A.Key + (B.Key - A.Key) * Alpha;
            Sample.Up            = Math::Normalize(LerpV(A.Up, B.Up, Alpha));
            Sample.Roll          = A.Roll + (B.Roll - A.Roll) * Alpha;
            Sample.Scale         = LerpV(A.Scale, B.Scale, Alpha);

            OutSamples.push_back(Sample);
        }

        return TotalLength;
    }

    FSplineSample SampleSplineAtDistance(const TVector<FSplineSample>& Samples, float TotalLength, float Distance)
    {
        if (Samples.empty())
        {
            return FSplineSample{};
        }

        const int32 Count = static_cast<int32>(Samples.size());
        if (Count == 1 || TotalLength <= 1.0e-6f)
        {
            return Samples[0];
        }

        const float Step  = TotalLength / static_cast<float>(Count - 1);
        const float Clamped = Math::Clamp(Distance, 0.0f, TotalLength);
        const float Slot  = Clamped / Step;

        const int32 Index = Math::Clamp(static_cast<int32>(Math::Floor(Slot)), 0, Count - 2);
        const float Alpha = Math::Clamp(Slot - static_cast<float>(Index), 0.0f, 1.0f);

        const FSplineSample& A = Samples[Index];
        const FSplineSample& B = Samples[Index + 1];

        FSplineSample Result;
        Result.Position      = LerpV(A.Position, B.Position, Alpha);
        Result.DistanceAlong = Clamped;
        Result.Tangent       = Math::Normalize(LerpV(A.Tangent, B.Tangent, Alpha));
        Result.Key           = A.Key + (B.Key - A.Key) * Alpha;
        Result.Up            = Math::Normalize(LerpV(A.Up, B.Up, Alpha));
        Result.Roll          = A.Roll + (B.Roll - A.Roll) * Alpha;
        Result.Scale         = LerpV(A.Scale, B.Scale, Alpha);
        return Result;
    }
}
