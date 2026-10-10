#pragma once
#include "Common.slang"
#include "SceneGlobals.slang"

// World space with the entity transform baked at extract. Distance sampling is constant speed, key evaluation is exact but not.

bool IsValidSpline(uint SplineIndex)
{
    return SplineIndex < NumSplines();
}

FGPUSpline* GetSpline(uint SplineIndex)
{
    return Splines().At(SplineIndex);
}

FGPUSplinePoint LoadSplinePoint(FGPUSpline* Spline, uint LocalPointIndex)
{
    const uint Clamped = min(LocalPointIndex, max(Spline.PointCount, 1u) - 1u);
    return SplinePoints().Get(Spline.PointOffset + Clamped);
}

FGPUSplineSample LoadSplineSample(FGPUSpline* Spline, uint LocalSampleIndex)
{
    const uint Clamped = min(LocalSampleIndex, max(Spline.SampleCount, 1u) - 1u);
    return SplineSamples().Get(Spline.SampleOffset + Clamped);
}

// Renormalized because lerping two unit vectors shortens them.
FGPUSplineSample LerpSplineSamples(FGPUSplineSample A, FGPUSplineSample B, float Alpha)
{
    FGPUSplineSample R;
    R.Position      = lerp(A.Position, B.Position, Alpha);
    R.DistanceAlong = lerp(A.DistanceAlong, B.DistanceAlong, Alpha);
    R.Tangent       = normalize(lerp(A.Tangent, B.Tangent, Alpha));
    R.Key           = lerp(A.Key, B.Key, Alpha);
    R.Up            = normalize(lerp(A.Up, B.Up, Alpha));
    R.Roll          = lerp(A.Roll, B.Roll, Alpha);
    R.Scale         = lerp(A.Scale, B.Scale, Alpha);
    R._Pad          = 0.0;
    return R;
}

// Clamped to [0, TotalLength], so past either end returns the end sample rather than wrapping.
FGPUSplineSample SampleSplineAtDistance(uint SplineIndex, float Distance)
{
    FGPUSplineSample Fallback;
    Fallback.Position      = float3(0.0);
    Fallback.DistanceAlong = 0.0;
    Fallback.Tangent       = float3(0.0, 0.0, 1.0);
    Fallback.Key           = 0.0;
    Fallback.Up            = float3(0.0, 1.0, 0.0);
    Fallback.Roll          = 0.0;
    Fallback.Scale         = float3(1.0);
    Fallback._Pad          = 0.0;

    if (!IsValidSpline(SplineIndex))
    {
        return Fallback;
    }

    FGPUSpline* Spline = GetSpline(SplineIndex);
    if (Spline.SampleCount == 0u)
    {
        return Fallback;
    }
    if (Spline.SampleCount == 1u || Spline.TotalLength <= 1e-6)
    {
        return LoadSplineSample(Spline, 0u);
    }

    // The table is uniform in distance, so the index is a divide with no search.
    const float Step  = Spline.TotalLength / float(Spline.SampleCount - 1u);
    const float Slot  = clamp(Distance, 0.0, Spline.TotalLength) / Step;
    const uint  Index = min((uint)floor(Slot), Spline.SampleCount - 2u);
    const float Alpha = saturate(Slot - float(Index));

    return LerpSplineSamples(LoadSplineSample(Spline, Index),
                             LoadSplineSample(Spline, Index + 1u),
                             Alpha);
}

FGPUSplineSample SampleSplineNormalized(uint SplineIndex, float Alpha)
{
    if (!IsValidSpline(SplineIndex))
    {
        return SampleSplineAtDistance(SplineIndex, 0.0);
    }

    return SampleSplineAtDistance(SplineIndex, saturate(Alpha) * GetSpline(SplineIndex).TotalLength);
}

float GetSplineLength(uint SplineIndex)
{
    return IsValidSpline(SplineIndex) ? GetSpline(SplineIndex).TotalLength : 0.0;
}

// T0 is the segment's leave tangent and T1 its arrive tangent, separate so a point can carry a corner.
float3 HermitePosition(float3 P0, float3 T0, float3 P1, float3 T1, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;

    return P0 * (2.0 * t3 - 3.0 * t2 + 1.0)
         + T0 * (t3 - 2.0 * t2 + t)
         + P1 * (-2.0 * t3 + 3.0 * t2)
         + T1 * (t3 - t2);
}

float3 HermiteDerivative(float3 P0, float3 T0, float3 P1, float3 T1, float t)
{
    const float t2 = t * t;

    return P0 * (6.0 * t2 - 6.0 * t)
         + T0 * (3.0 * t2 - 4.0 * t + 1.0)
         + P1 * (-6.0 * t2 + 6.0 * t)
         + T1 * (3.0 * t2 - 2.0 * t);
}

// Not constant speed. Reads the control points directly, independent of the baked table's density.
float3 EvaluateSplineAtKey(uint SplineIndex, float Key)
{
    if (!IsValidSpline(SplineIndex))
    {
        return float3(0.0);
    }

    FGPUSpline* Spline = GetSpline(SplineIndex);
    const uint Segments = Spline.NumSegments;
    if (Segments == 0u)
    {
        return Spline.PointCount > 0u ? LoadSplinePoint(Spline, 0u).Location : float3(0.0);
    }

    const float Clamped = clamp(Key, 0.0, float(Segments));
    const uint  Segment = min((uint)floor(Clamped), Segments - 1u);
    const float t       = saturate(Clamped - float(Segment));

    // The closing segment of a loop wraps back to point 0, hence the modulo.
    const FGPUSplinePoint A = LoadSplinePoint(Spline, Segment);
    const FGPUSplinePoint B = LoadSplinePoint(Spline, (Segment + 1u) % Spline.PointCount);

    return HermitePosition(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, t);
}

// Unnormalized, so normalize it for a direction.
float3 EvaluateSplineTangentAtKey(uint SplineIndex, float Key)
{
    if (!IsValidSpline(SplineIndex))
    {
        return float3(0.0, 0.0, 1.0);
    }

    FGPUSpline* Spline = GetSpline(SplineIndex);
    const uint Segments = Spline.NumSegments;
    if (Segments == 0u)
    {
        return float3(0.0, 0.0, 1.0);
    }

    const float Clamped = clamp(Key, 0.0, float(Segments));
    const uint  Segment = min((uint)floor(Clamped), Segments - 1u);
    const float t       = saturate(Clamped - float(Segment));

    const FGPUSplinePoint A = LoadSplinePoint(Spline, Segment);
    const FGPUSplinePoint B = LoadSplinePoint(Spline, (Segment + 1u) % Spline.PointCount);

    return HermiteDerivative(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, t);
}

// Roll is already baked into the sample's Up, so this is a plain Gram-Schmidt.
float3x3 SplineSampleFrame(FGPUSplineSample S)
{
    const float3 Forward = S.Tangent;
    const float3 Up      = S.Up;
    const float3 Right   = normalize(cross(Up, Forward));
    // Re-derived so the frame stays orthonormal when the lerped Up drifts.
    const float3 TrueUp  = cross(Forward, Right);

    return float3x3(Right, TrueUp, Forward);
}

// O(SampleCount) scan of the baked table, so avoid calling it in a loop over a dense table.
float FindNearestDistanceOnSpline(uint SplineIndex, float3 WorldPos, out float OutDistanceSq)
{
    OutDistanceSq = 1e30;

    if (!IsValidSpline(SplineIndex))
    {
        return 0.0;
    }

    FGPUSpline* Spline = GetSpline(SplineIndex);
    if (Spline.SampleCount == 0u)
    {
        return 0.0;
    }

    uint  BestIndex = 0u;
    float BestDistSq = 1e30;

    for (uint i = 0u; i < Spline.SampleCount; ++i)
    {
        const uint SampleIndex = Spline.SampleOffset + i;
        if (!SplineSamples().Contains(SampleIndex))
        {
            continue;
        }

        const float3 Delta = SplineSamples().At(SampleIndex).Position - WorldPos;
        const float  DSq   = dot(Delta, Delta);
        if (DSq < BestDistSq)
        {
            BestDistSq = DSq;
            BestIndex  = i;
        }
    }

    // Project onto the two adjacent segments, or a coarse table quantizes the result to the sample spacing.
    const FGPUSplineSample Best = LoadSplineSample(Spline, BestIndex);
    float BestDistance = Best.DistanceAlong;
    OutDistanceSq = BestDistSq;

    for (int Side = -1; Side <= 1; Side += 2)
    {
        const int Neighbour = int(BestIndex) + Side;
        if (Neighbour < 0 || Neighbour >= int(Spline.SampleCount))
        {
            continue;
        }

        const FGPUSplineSample N = LoadSplineSample(Spline, uint(Neighbour));
        const float3 Segment = N.Position - Best.Position;
        const float  LenSq   = dot(Segment, Segment);
        if (LenSq <= 1e-12)
        {
            continue;
        }

        const float  T   = saturate(dot(WorldPos - Best.Position, Segment) / LenSq);
        const float3 Hit = Best.Position + Segment * T;
        const float3 D   = Hit - WorldPos;
        const float  DSq = dot(D, D);

        if (DSq < OutDistanceSq)
        {
            OutDistanceSq = DSq;
            BestDistance  = lerp(Best.DistanceAlong, N.DistanceAlong, T);
        }
    }

    return BestDistance;
}
