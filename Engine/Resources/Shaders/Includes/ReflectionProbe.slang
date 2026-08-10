#pragma once
#include "SceneGlobals.slang"

// Every probe's volume is the unit box or sphere in ITS probe space -- extraction bakes the world
// half-extents into WorldToProbe, so the inside test, blend and parallax need no per-probe extents.

// Probes are sorted by descending priority on extraction, so a plain front-to-back walk lets a small
// high-priority interior probe consume weight before the large one enclosing it ever sees the pixel.
static const uint kMaxProbesPerPixel = 8;

struct FProbeSample
{
    float3 Radiance;
    float  Weight;    // total influence consumed, in [0,1]; the remainder belongs to the sky
};

// Normalized so 0 is exactly on the surface and 1 the deepest interior. Box uses Chebyshev distance
// (its iso-surfaces are nested boxes), sphere the Euclidean one.
float Probe_BoundaryDistance(FGPUReflectionProbe Probe, float3 LocalPos)
{
    if (Probe.IsSphere)
    {
        return 1.0 - length(LocalPos);
    }
    float3 D = abs(LocalPos);
    return 1.0 - max(max(D.x, D.y), D.z);
}

// 0 at the boundary rising to 1 once BlendFraction inside. Smoothstep rather than linear so
// overlapping probes cross-fade without a gradient discontinuity where one edge cuts another.
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

// Without this every probe behaves like an infinitely distant cubemap: reflections slide across
// surfaces as the camera moves instead of staying pinned to the geometry that produced them.
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
        // Slab test against [-1,1]^3. A zero LocalDir component is deliberately NOT guarded: it yields +-inf,
        // which max/min discard. The 0*inf NaN needs LocalPos exactly on the boundary, unreachable here.
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

// Mirrors SampleReflectionProbes' skip rules exactly, so the debug view cannot claim coverage shading
// did not get. Hues are offset and PASTEL so they can never be read as the saturated status codes.
float3 ProbeDebugColor(uint Slice)
{
    float  Hue = frac(0.37 + float(Slice) * 0.61803398875);
    float3 K   = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    float3 P   = abs(frac(Hue + K) * 6.0 - 3.0);
    float3 RGB = saturate(P - 1.0);

    // S ~= 0.7: pastel enough to never read as one of the saturated status colors.
    return lerp(float3(1.0), RGB, 0.7);
}

// Why a pixel got no probe influence. All of these look identical in a lit view (the surface just
// reflects sky) but have different causes, so they get distinct colors rather than one black.
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
