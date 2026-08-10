#pragma once
#include "Common.slang"
#include "SceneGlobals.slang"

// Sampling for splines uploaded by SSplineComponent (bSendToGPU).
//
// Two ways to read a spline, and they answer different questions:
//
//   * SampleSplineAtDistance -- constant speed. Distance is world-space arc length in [0, TotalLength].
//     Backed by the baked table, so it is one divide plus one lerp. This is what you almost always want
//     for placing things along a curve, marching a ray, or driving movement.
//
//   * EvaluateSplineAtKey -- the exact curve. Key is in [0, NumSegments]; the integer part picks the
//     segment and the fraction is position within it. Exact but NOT constant speed: equal key steps cover
//     unequal distances. Use when you need the analytic curve or its derivative.
//
// Everything is world space; the entity transform was baked in at extract.

bool IsValidSpline(uint SplineIndex)
{
    return SplineIndex < NumSplines();
}

FGPUSpline* GetSpline(uint SplineIndex)
{
    return &Splines()[SplineIndex];
}

FGPUSplinePoint LoadSplinePoint(FGPUSpline* Spline, uint LocalPointIndex)
{
    const uint Clamped = min(LocalPointIndex, max(Spline.PointCount, 1u) - 1u);
    return SplinePoints()[Spline.PointOffset + Clamped];
}

FGPUSplineSample LoadSplineSample(FGPUSpline* Spline, uint LocalSampleIndex)
{
    const uint Clamped = min(LocalSampleIndex, max(Spline.SampleCount, 1u) - 1u);
    return SplineSamples()[Spline.SampleOffset + Clamped];
}

// Lerp two samples. Tangent/Up are renormalised: lerping two unit vectors shortens them, and a
// consumer building a frame from these needs them unit-length.
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

/**
 * Constant-speed sample at a world-space distance along the curve. Distance is clamped to
 * [0, TotalLength] -- past either end you get the end sample, not a wrap.
 */
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

    // The table is uniform in distance, so the index is just a divide -- no search, no dependent loads.
    const float Step  = Spline.TotalLength / float(Spline.SampleCount - 1u);
    const float Slot  = clamp(Distance, 0.0, Spline.TotalLength) / Step;
    const uint  Index = min((uint)floor(Slot), Spline.SampleCount - 2u);
    const float Alpha = saturate(Slot - float(Index));

    return LerpSplineSamples(LoadSplineSample(Spline, Index),
                             LoadSplineSample(Spline, Index + 1u),
                             Alpha);
}

/** Same, but with a normalised position in [0,1] along the curve. */
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

// Cubic Hermite. T0 is the segment's LEAVE tangent, T1 its ARRIVE tangent -- separate so a point can
// carry a corner.
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

/**
 * Exact curve position at a key in [0, NumSegments]. NOT constant speed -- see the header comment.
 * Reads the control points directly, so it does not depend on the baked table's density.
 */
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

/** Exact (unnormalised) derivative at a key. Normalize it yourself if you want a direction. */
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

/**
 * Orthonormal frame at a sample: right/up/forward, with forward along the curve.
 * Roll is already baked into the sample's Up by the extract, so this is a plain Gram-Schmidt.
 */
float3x3 SplineSampleFrame(FGPUSplineSample S)
{
    const float3 Forward = S.Tangent;
    const float3 Up      = S.Up;
    const float3 Right   = normalize(cross(Up, Forward));
    // Re-derive Up from the orthogonalised Right so the frame is exactly orthonormal even if the lerped
    // Up drifted slightly off perpendicular between two table entries.
    const float3 TrueUp  = cross(Forward, Right);

    return float3x3(Right, TrueUp, Forward);
}

/**
 * Nearest point on a spline to a world position, by scanning the baked table.
 * Returns the distance-along of the closest sample refined against its neighbours; OutDistanceSq is the
 * squared world distance to the curve. O(SampleCount) -- fine for one query per thread against a
 * moderate table, but do not call it in a loop over a dense table.
 */
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
        const float3 Delta = SplineSamples()[Spline.SampleOffset + i].Position - WorldPos;
        const float  DSq   = dot(Delta, Delta);
        if (DSq < BestDistSq)
        {
            BestDistSq = DSq;
            BestIndex  = i;
        }
    }

    // Refine within the two adjacent segments by projecting onto them -- a coarse table otherwise
    // quantises the result to the sample spacing.
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
