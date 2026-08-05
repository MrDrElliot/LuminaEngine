#pragma once
#include "SceneGlobals.slang"

// Local reflection probes: parallax-corrected specular from cubemaps captured in the world, blended
// front-to-back by influence and falling back to the sky prefilter wherever they don't cover.
//
// Every probe's volume is the unit box [-1,1]^3 (or the unit sphere) in ITS probe space, because
// extraction bakes the world half-extents into WorldToProbe. That one convention is what lets the
// inside test, the blend weight, and the parallax intersection all run without per-probe extents.
// A ray-volume intersection is invariant under the linear part of a transform, so the world
// reflection vector can be carried into probe space unnormalized and the resulting t used as-is.

// Probes are sorted by descending priority on extraction, so a plain front-to-back walk lets a small
// high-priority interior probe consume weight before the large one enclosing it ever sees the pixel.
static const uint kMaxProbesPerPixel = 8;

struct FProbeSample
{
    float3 Radiance;
    float  Weight;    // total influence consumed, in [0,1]; the remainder belongs to the sky
};

// Distance from the volume boundary, normalized so 0 is exactly on the surface and 1 is the
// deepest interior. Box uses the Chebyshev distance (its iso-surfaces are nested boxes), sphere
// the Euclidean one.
float Probe_BoundaryDistance(FGPUReflectionProbe Probe, float3 LocalPos)
{
    if (Probe.IsSphere)
    {
        return 1.0 - length(LocalPos);
    }
    float3 D = abs(LocalPos);
    return 1.0 - max(max(D.x, D.y), D.z);
}

// Influence ramp: 0 at the boundary rising to 1 once BlendFraction of the way inside. Smoothstep
// rather than linear so overlapping probes cross-fade without a visible gradient discontinuity
// where one volume's edge cuts across another's interior.
float Probe_InfluenceWeight(FGPUReflectionProbe Probe, float3 LocalPos)
{
    float Depth = Probe_BoundaryDistance(Probe, LocalPos);
    if (Depth <= 0.0)
    {
        return 0.0;
    }
    float Blend = max(Probe.BlendFraction, 1e-3);
    return smoothstep(0.0, Blend, Depth);
}

// Reproject the reflection ray onto the probe's proxy volume. Without this every probe behaves like
// an infinitely distant cubemap: reflections slide across surfaces as the camera moves instead of
// staying pinned to the geometry that produced them, which is the entire reason local probes exist.
float3 Probe_ParallaxDirection(FGPUReflectionProbe Probe, float3 LocalPos, float3 WorldR)
{
    float3 LocalDir = mul((float3x3)Probe.WorldToProbe, WorldR);

    float T;
    if (Probe.IsSphere)
    {
        // Ray vs unit sphere at the origin; LocalPos is inside, so the positive root is the exit.
        float A = dot(LocalDir, LocalDir);
        float B = dot(LocalPos, LocalDir);
        float C = dot(LocalPos, LocalPos) - 1.0;
        float Disc = B * B - A * C;
        T = (-B + sqrt(max(Disc, 0.0))) / max(A, 1e-6);
    }
    else
    {
        // Slab test against [-1,1]^3. A component of LocalDir being zero is deliberately NOT guarded:
        // it yields +-inf for that slab, which max() then min() discards, and that is the standard
        // robust form. The 0 * inf = NaN case needs LocalPos exactly on the boundary, which cannot
        // reach here because a caller only parallax-corrects a probe whose influence is already > 0
        // (strictly inside), leaving both (-1 - LocalPos) and (1 - LocalPos) non-zero.
        float3 InvDir = 1.0 / LocalDir;
        float3 T1 = (-1.0 - LocalPos) * InvDir;
        float3 T2 = ( 1.0 - LocalPos) * InvDir;
        float3 TMax = max(T1, T2);
        T = min(min(TMax.x, TMax.y), TMax.z);
    }

    float3 HitLocal = LocalPos + LocalDir * max(T, 0.0);
    float3 HitWorld = mul(Probe.ProbeToWorld, float4(HitLocal, 1.0)).xyz;

    // The cube was captured from CapturePosition in world orientation, so the lookup direction is
    // from the capture origin toward where the ray actually lands on the proxy.
    return HitWorld - Probe.CapturePosition.xyz;
}

// Walks the probe list front-to-back, consuming influence until it runs out. Returns the accumulated
// radiance and how much of the pixel it covered; the caller blends the sky in for the remainder.
FProbeSample SampleReflectionProbes(float3 WorldPos, float3 WorldR, float Roughness)
{
    FProbeSample Result;
    Result.Radiance = float3(0.0);
    Result.Weight   = 0.0;

    uint Packed    = GetSceneRoot().ProbeCubeArrayIndex;
    uint ArraySRV  = Packed & 0x00FFFFFFu;
    uint NumMips   = Packed >> 24;
    uint NumProbes = GetSceneRoot().NumReflectionProbes;

    if (NumProbes == 0 || NumMips == 0)
    {
        return Result;
    }

    float MaxLod = max(float(NumMips) - 1.0, 0.0);
    float Lod    = saturate(Roughness) * MaxLod;

    float Remaining = 1.0;
    uint  Considered = 0;

    for (uint i = 0; i < NumProbes && Considered < kMaxProbesPerPixel; ++i)
    {
        FGPUReflectionProbe Probe = GetSceneRoot().ReflectionProbesPtr[i];
        if (!Probe.IsBaked)
        {
            continue;
        }

        float3 LocalPos = mul(Probe.WorldToProbe, float4(WorldPos, 1.0)).xyz;

        float Influence = Probe_InfluenceWeight(Probe, LocalPos);
        if (Influence <= 0.0)
        {
            continue;
        }
        Considered++;

        // Front-to-back: each probe can only claim what higher-priority ones left behind.
        float Weight = min(Influence, Remaining);

        float3 Dir = Probe_ParallaxDirection(Probe, LocalPos, WorldR);
        float3 Radiance = SampleTextureCubeArrayLevel(ArraySRV, SAMPLER_LINEAR_CLAMP, Dir, Probe.Slice, Lod).rgb;

        Result.Radiance += Weight * Radiance * Probe.Brightness;
        Remaining       -= Weight;

        if (Remaining <= 1e-3)
        {
            break;
        }
    }

    Result.Weight = saturate(1.0 - Remaining);
    return Result;
}

// Debug readout of the same walk SampleReflectionProbes performs. Deliberately mirrors its skip rules
// and front-to-back consumption exactly, so the debug view can never claim coverage that shading did
// not actually get.
// Distinct, always-bright hue per probe slice.
//
// Deliberately NOT the integer hash used for meshlet/material debug colors: that hash is purely
// multiplicative, so index 0 maps to 0 and renders PURE BLACK, and other indices can land arbitrarily
// dark. For a view whose whole job is distinguishing "a probe covers this" from "nothing covers this",
// a palette that can emit black is worse than useless -- it reports success as failure.
//
// Golden-ratio hue stepping keeps adjacent slices far apart in hue.
//
// Two deliberate choices keep probe hues from being mistaken for the status codes below, which are
// fully-saturated primaries. Getting this wrong is not cosmetic: slice 0 originally landed on hue 0,
// i.e. PURE RED, which is byte-identical to the "no probes at all" status -- so a perfectly working
// probe reported itself as total failure.
//   1. A hue offset, so slice 0 is not a primary.
//   2. Desaturation toward white, so every probe hue is visibly PASTEL while every status color stays
//      fully saturated. That makes the distinction structural (saturation) rather than a hue lottery
//      that a large enough probe count would eventually lose.
float3 ProbeDebugColor(uint Slice)
{
    float  Hue = frac(0.37 + float(Slice) * 0.61803398875);
    float3 K   = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    float3 P   = abs(frac(Hue + K) * 6.0 - 3.0);
    float3 RGB = saturate(P - 1.0);

    // S ~= 0.7: pastel enough to never read as one of the saturated status colors.
    return lerp(float3(1.0), RGB, 0.7);
}

// Why a pixel ended up with no probe influence. Every one of these looks identical in a lit view
// (the surface just reflects sky), but they have completely different causes, so the debug view
// reports them as distinct colors rather than a single uninformative black.
static const int kProbeStatusCovered    = 0;  // a probe reached this pixel
static const int kProbeStatusNoProbes   = 1;  // scene root carries no probes at all
static const int kProbeStatusNoArray    = 2;  // probe cube array unallocated / mip count zero
static const int kProbeStatusNoneBaked  = 3;  // probes exist, none has finished a capture
static const int kProbeStatusOutsideAll = 4;  // baked probes exist, none of their volumes contain this pixel

struct FProbeDebugInfo
{
    int   DominantSlice;   // slice of the heaviest contributor, -1 when no probe reaches this pixel
    float TotalWeight;     // influence the probes consumed; 1 - this is the sky's share
    int   NumAffecting;    // how many contributed, for spotting unintended overlap
    int   Status;          // kProbeStatus*
};

FProbeDebugInfo DebugReflectionProbes(float3 WorldPos)
{
    FProbeDebugInfo Info;
    Info.DominantSlice = -1;
    Info.TotalWeight   = 0.0;
    Info.NumAffecting  = 0;
    Info.Status        = kProbeStatusCovered;

    uint NumProbes = GetSceneRoot().NumReflectionProbes;
    uint NumMips   = GetSceneRoot().ProbeCubeArrayIndex >> 24;
    if (NumProbes == 0)
    {
        Info.Status = kProbeStatusNoProbes;
        return Info;
    }
    if (NumMips == 0)
    {
        Info.Status = kProbeStatusNoArray;
        return Info;
    }

    float Remaining   = 1.0;
    float BestWeight  = 0.0;
    uint  Considered  = 0;
    int   NumBaked    = 0;

    for (uint i = 0; i < NumProbes && Considered < kMaxProbesPerPixel; ++i)
    {
        FGPUReflectionProbe Probe = GetSceneRoot().ReflectionProbesPtr[i];
        if (!Probe.IsBaked)
        {
            continue;
        }
        NumBaked++;

        float3 LocalPos  = mul(Probe.WorldToProbe, float4(WorldPos, 1.0)).xyz;
        float  Influence = Probe_InfluenceWeight(Probe, LocalPos);
        if (Influence <= 0.0)
        {
            continue;
        }
        Considered++;

        float Weight = min(Influence, Remaining);
        Remaining   -= Weight;
        Info.NumAffecting++;

        if (Weight > BestWeight)
        {
            BestWeight         = Weight;
            Info.DominantSlice = (int)Probe.Slice;
        }

        if (Remaining <= 1e-3)
        {
            break;
        }
    }

    Info.TotalWeight = saturate(1.0 - Remaining);
    if (Info.DominantSlice < 0)
    {
        Info.Status = (NumBaked == 0) ? kProbeStatusNoneBaked : kProbeStatusOutsideAll;
    }
    return Info;
}
