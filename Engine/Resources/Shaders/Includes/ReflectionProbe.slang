#pragma once
#include "SceneGlobals.slang"

// Each probe volume is the unit box or sphere in its own probe space, with world half-extents baked into WorldToProbe.

// Probes arrive sorted by descending priority, so a front-to-back walk lets a small interior probe claim weight first.
static const uint kMaxProbesPerPixel = 8;

struct FProbeSample
{
    float3 Radiance;
    float  Weight;    // total influence consumed, in [0,1]; the remainder belongs to the sky
};

float3 Probe_ClampToVolume(FGPUReflectionProbe Probe, float3 LocalPos)
{
    if (Probe.IsSphere)
    {
        return LocalPos / max(length(LocalPos), 1.0);
    }
    return clamp(LocalPos, -1.0, 1.0);
}

// Full weight inside, so a box fitted to a room owns its own walls, then a fade over BlendDistance meters outside it.
float Probe_InfluenceWeight(FGPUReflectionProbe Probe, float3 LocalPos)
{
    float Outside = length(mul((float3x3)Probe.ProbeToWorld, LocalPos - Probe_ClampToVolume(Probe, LocalPos)));
    return 1.0 - smoothstep(0.0, max(Probe.BlendDistance, 1e-3), Outside);
}

// Parallax correction, so reflections stay pinned to the geometry that produced them.
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
        // A zero LocalDir component yields +-inf, which max and min discard, so it is left unguarded.
        float3 InvDir = 1.0 / LocalDir;
        float3 T1 = (-1.0 - LocalPos) * InvDir;
        float3 T2 = ( 1.0 - LocalPos) * InvDir;
        float3 TMax = max(T1, T2);
        T = min(min(TMax.x, TMax.y), TMax.z);
    }

    float3 HitLocal = LocalPos + LocalDir * max(T, 0.0);
    float3 HitWorld = mul(Probe.ProbeToWorld, float4(HitLocal, 1.0)).xyz;

    // The cube was captured from CapturePosition, so look up from there toward the proxy hit.
    return HitWorld - Probe.CapturePosition.xyz;
}

// Returns radiance and covered weight; the caller blends the sky in for the remainder.
FProbeSample SampleReflectionProbes(float3 WorldPos, float3 WorldR, float Roughness)
{
    FProbeSample Result;
    Result.Radiance = float3(0.0);
    Result.Weight   = 0.0;

    const FMippedTextureCubeArray Probes = GetSceneRoot().ProbeCubeArray;
    uint NumMips   = Probes.MipCount();
    uint NumProbes = ReflectionProbes().Count;

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
        FGPUReflectionProbe Probe = ReflectionProbes().GetAligned(i);
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

        // Each probe can only claim what higher-priority ones left behind.
        float Weight = min(Influence, Remaining);

        // A pixel in the fade band lies outside the proxy, so it traces from the nearest point on it.
        float3 Dir = Probe_ParallaxDirection(Probe, Probe_ClampToVolume(Probe, LocalPos), WorldR);
        float3 Radiance = Probes.Texture().SampleLevel(Samplers::LinearClamp, Dir, Probe.Slice, Lod).rgb;

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

// Hues are offset and pastel so they never read as the saturated status codes.
float3 ProbeDebugColor(uint Slice)
{
    float  Hue = frac(0.37 + float(Slice) * 0.61803398875);
    float3 K   = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    float3 P   = abs(frac(Hue + K) * 6.0 - 3.0);
    float3 RGB = saturate(P - 1.0);

    return lerp(float3(1.0), RGB, 0.7);
}

// Distinct colors for causes that all look like plain sky reflection in a lit view.
static const int kProbeStatusCovered    = 0;  // a probe reached this pixel
static const int kProbeStatusNoProbes   = 1;  // scene root carries no probes at all
static const int kProbeStatusNoArray    = 2;  // probe cube array unallocated / mip count zero
static const int kProbeStatusNoneBaked  = 3;  // probes exist, none has finished a capture
static const int kProbeStatusOutsideAll = 4;  // baked probes exist, none of their volumes contain this pixel

struct FProbeDebugInfo
{
    int   DominantSlice;   // slice of the heaviest contributor, -1 when no probe reaches this pixel
    float TotalWeight;     // influence the probes consumed, the sky takes the rest
    int   NumAffecting;    // how many contributed, for spotting unintended overlap
    int   Status;          // kProbeStatus*
};

// Mirrors SampleReflectionProbes' skip rules so the debug view cannot claim coverage shading did not get.
FProbeDebugInfo DebugReflectionProbes(float3 WorldPos)
{
    FProbeDebugInfo Info;
    Info.DominantSlice = -1;
    Info.TotalWeight   = 0.0;
    Info.NumAffecting  = 0;
    Info.Status        = kProbeStatusCovered;

    uint NumProbes = ReflectionProbes().Count;
    uint NumMips   = GetSceneRoot().ProbeCubeArray.MipCount();
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
        FGPUReflectionProbe Probe = ReflectionProbes().GetAligned(i);
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
