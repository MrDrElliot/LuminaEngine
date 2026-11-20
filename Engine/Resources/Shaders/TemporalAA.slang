#include "Includes/SceneGlobals.slang"
#include "Includes/CatmullRom.slang"

// Temporal anti-aliasing and upscaling, which rebuilds this frame's jittered samples under each display pixel and blends them into the reprojected, clipped history.

struct FData
{
    FTexture2D   Color;
    FTexture2D   Depth;
    FTexture2D   Velocity;
    FTexture2D   History;
    FRWTexture2D Output;
    FRWTexture2D HistoryOut;
    uint2        RenderSize;
    uint2        DisplaySize;
    float2       JitterUV;
    uint         bReset;
    float        ClipGamma;
    float        MinCurrentWeight;
    uint         _Pad0;
};
FData* Pass() { return GetPassArgs<FData>(); }

// The current share a fast mover climbs to, since a history resampled every frame softens.
static const float kMovingCurrentWeight = 0.25;
// Pixels of motion over which the current share climbs to it.
static const float kMovingPixels = 16.0;
// Blackman-Harris fitted by a Gaussian, per squared render pixel of distance.
static const float kSampleFalloff = 2.29;

float3 RGBToYCoCg(float3 C)
{
    return float3(dot(C, float3(0.25, 0.5, 0.25)), dot(C, float3(0.5, 0.0, -0.5)), dot(C, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRGB(float3 C)
{
    return float3(C.x + C.y - C.z, C.x + C.z, C.x - C.y - C.z);
}

// Blended under a reversible tonemap, so one bright sample cannot dominate its neighbors and flicker.
float3 ToneWeight(float3 C)
{
    return C / (1.0 + max(C.r, max(C.g, C.b)));
}

float3 InverseToneWeight(float3 C)
{
    return C / max(1.0 - max(C.r, max(C.g, C.b)), 1.0 / 65504.0);
}

float3 Sanitize(float3 C)
{
    return any(isnan(C)) || any(isinf(C)) ? float3(0.0, 0.0, 0.0) : max(C, float3(0.0, 0.0, 0.0));
}

// Pulled toward the box center until inside it, which keeps the history's hue where a clamp per channel would shift it.
float3 ClipToBox(float3 History, float3 Center, float3 Extent)
{
    const float3 Offset  = History - Center;
    const float3 Units   = abs(Offset / max(Extent, float3(1e-5, 1e-5, 1e-5)));
    const float  MaxUnit = max(Units.x, max(Units.y, Units.z));
    return MaxUnit > 1.0 ? Center + Offset / MaxUnit : History;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 Tid : SV_DispatchThreadID)
{
    FData* P = Pass();
    if (any(Tid.xy >= P.DisplaySize))
    {
        return;
    }

    const float2 RenderSize = float2(P.RenderSize);
    const float2 DisplayUV  = (float2(Tid.xy) + 0.5) / float2(P.DisplaySize);
    const float2 RenderPos  = DisplayUV * RenderSize;

    // Render texel k saw the scene at k + 0.5 minus the jitter, so the nearest sample sits under RenderPos plus the jitter.
    const float2 JitterPixels = P.JitterUV * RenderSize;
    const int2   Base         = int2(floor(RenderPos + JitterPixels));
    const int2   MaxTexel     = int2(P.RenderSize) - int2(1, 1);

    float3 Filtered     = float3(0.0, 0.0, 0.0);
    float  WeightSum    = 0.0;
    float  Coverage     = 0.0;
    float3 Moment1      = float3(0.0, 0.0, 0.0);
    float3 Moment2      = float3(0.0, 0.0, 0.0);
    float  ClosestDepth = -1.0;
    int2   ClosestTexel = clamp(Base, int2(0, 0), MaxTexel);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2   Texel  = clamp(Base + int2(x, y), int2(0, 0), MaxTexel);
            const float3 Sample = ToneWeight(Sanitize(P.Color.Load(Texel).rgb));
            const float2 Offset = float2(Texel) + 0.5 - JitterPixels - RenderPos;
            const float  Weight = exp(-kSampleFalloff * dot(Offset, Offset));

            Filtered  += Sample * Weight;
            WeightSum += Weight;
            Coverage   = max(Coverage, Weight);

            const float3 YCoCg = RGBToYCoCg(Sample);
            Moment1 += YCoCg;
            Moment2 += YCoCg * YCoCg;

            // Reverse-Z, so the largest depth is the nearest surface, whose motion the silhouette should follow.
            const float Depth = P.Depth.Load(Texel).r;
            if (Depth > ClosestDepth)
            {
                ClosestDepth = Depth;
                ClosestTexel = Texel;
            }
        }
    }

    const float3 Current = RGBToYCoCg(Filtered / max(WeightSum, 1e-5));

    const float2 Motion    = P.Velocity.Load(ClosestTexel).rg;
    const float2 HistoryUV = DisplayUV + Motion;

    float3 Result = Current;
    if (P.bReset == 0u && all(saturate(HistoryUV) == HistoryUV))
    {
        const float3 Mean   = Moment1 / 9.0;
        const float3 Sigma  = sqrt(max(Moment2 / 9.0 - Mean * Mean, float3(0.0, 0.0, 0.0)));
        const float3 Stored = Sanitize(SampleCatmullRom(P.History, Samplers::LinearClamp, HistoryUV, float2(P.DisplaySize)));
        const float3 History = ClipToBox(RGBToYCoCg(ToneWeight(Stored)), Mean, Sigma * P.ClipGamma);

        // A pixel no sample landed near trusts its history more, which is what lets an upscale gather detail over frames.
        const float MotionPixels = length(Motion * float2(P.DisplaySize));
        const float Moving       = lerp(P.MinCurrentWeight, kMovingCurrentWeight, saturate(MotionPixels / kMovingPixels));
        const float CurrentShare = saturate(Moving * Coverage);
        Result = lerp(History, Current, CurrentShare);
    }

    const float4 Final = float4(InverseToneWeight(YCoCgToRGB(Result)), 1.0);
    P.Output[Tid.xy]     = Final;
    P.HistoryOut[Tid.xy] = Final;
}
