#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/XeGTAO.slang"

// Half resolution indirect diffuse, rgb the bounced light and a the cosine-weighted fraction of the hemisphere it came from.

#ifndef SSGI_DENOISE

struct FPushConstants
{
    float4x4     HistoryViewProjection;
    uint2        OutputSize;
    uint2        WorkingDepthSize;
    FTexture2D   WorkingDepth;
    FTexture2D   SceneDepth;
    FTexture2D   SceneColorHistory;
    FRWTexture2D Output;
    uint         DepthScale;
    uint         SliceCount;
    uint         StepsPerSlice;
    float        EffectRadius;
    float        EffectFalloffRange;
    float        SampleDistributionPower;
    float        ThinOccluderCompensation;
    float        DepthMIPSamplingOffset;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Last frame's color at the surface under UV, or black where that surface was off screen or behind the eye.
float3 HitRadiance(float2 UV)
{
    const float Raw = Pass().SceneDepth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
    if (Raw <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float4 PrevClip = mul(Pass().HistoryViewProjection, float4(ReconstructWorldPos(UV, Raw), 1.0));
    if (PrevClip.w <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float2 PrevUV = (PrevClip.xy / PrevClip.w) * 0.5 + 0.5;
    if (any(PrevUV != saturate(PrevUV)))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return Pass().SceneColorHistory.SampleLevel(Samplers::LinearClamp, PrevUV, 0.0).rgb;
}

[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 PixCoord = DTid.xy;
    if (any(PixCoord >= Pass().OutputSize))
    {
        return;
    }

    const FGTAOView   V          = MakeGTAOView(Pass().WorkingDepthSize);
    const uint2       DepthCoord = min(PixCoord * Pass().DepthScale, Pass().WorkingDepthSize - 1u);
    const float2      ScreenUV   = (float2(DepthCoord) + 0.5) * V.ViewportPixelSize;
    const FGTAOCenter Center     = XeGTAO_LoadCenter(V, Pass().WorkingDepth, DepthCoord);

    // The same nudge GTAOMain makes, so a texel never finds its own depth above its horizon.
    const float  ViewspaceZ = Center.ViewspaceZ * 0.99999;
    const float3 CenterPos  = XeGTAO_ComputeViewspacePosition(V, ScreenUV, ViewspaceZ);
    const float3 ViewVec    = normalize(-CenterPos);

    const float ScreenspaceRadius = Pass().EffectRadius / max(abs(V.NDCToViewMulPixelSize.x * ViewspaceZ), 1e-8);
    if (ScreenspaceRadius < XE_GTAO_PIXEL_TOO_CLOSE)
    {
        Pass().Output[PixCoord] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const FGTAOFalloff Falloff = XeGTAO_MakeFalloff(Pass().EffectRadius, Pass().EffectFalloffRange, Pass().ThinOccluderCompensation);
    const float2       Noise   = XeGTAO_SpatioTemporalNoise(PixCoord, 0u);
    const float        MinS    = XE_GTAO_PIXEL_TOO_CLOSE / ScreenspaceRadius;

    const uint SliceCount    = max(Pass().SliceCount, 1u);
    const uint StepsPerSlice = max(Pass().StepsPerSlice, 1u);

    float3 Bounce   = float3(0.0, 0.0, 0.0);
    float  Coverage = 0.0;

    [loop]
    for (uint Slice = 0; Slice < SliceCount; ++Slice)
    {
        const FGTAOSlice S = XeGTAO_MakeSlice(V, (float(Slice) + Noise.x) / float(SliceCount) * XE_GTAO_PI,
                                              ScreenspaceRadius, ViewVec, Center.Normal);

        // Side 0 marches along Omega and opens toward positive angles, side 1 the mirror of it.
        float LowCos[2]     = { S.LowHorizonCos0, S.LowHorizonCos1 };
        float HorizonCos[2] = { S.LowHorizonCos0, S.LowHorizonCos1 };
        float Arc[2]        = { XeGTAO_SliceArc(S, XeGTAO_FastACos(S.LowHorizonCos0)),
                                XeGTAO_SliceArc(S, -XeGTAO_FastACos(S.LowHorizonCos1)) };

        float3 SliceBounce   = float3(0.0, 0.0, 0.0);
        float  SliceCoverage = 0.0;

        [loop]
        for (uint Step = 0; Step < StepsPerSlice; ++Step)
        {
            const float Distance = XeGTAO_StepDistance(Slice, Step, StepsPerSlice, Noise.y, Pass().SampleDistributionPower, MinS);

            float MipLevel;
            const float2 SampleOffset = XeGTAO_StepOffsetUV(V, S, Distance, Pass().DepthMIPSamplingOffset, MipLevel);

            [unroll]
            for (uint Side = 0; Side < 2; ++Side)
            {
                const float2 SampleUV = (Side == 0u) ? ScreenUV + SampleOffset : ScreenUV - SampleOffset;
                const float  SampleZ  = Pass().WorkingDepth.SampleLevel(Samplers::PointClamp, SampleUV, MipLevel).x;
                const float3 Delta    = XeGTAO_ComputeViewspacePosition(V, SampleUV, SampleZ) - CenterPos;
                const float  SampleCos = XeGTAO_SampleHorizonCos(Falloff, Delta, ViewVec, LowCos[Side]);
                if (SampleCos <= HorizonCos[Side])
                {
                    continue;
                }

                // Only the arc a sample newly hides above the horizon so far is light it can bounce in.
                const float Angle   = XeGTAO_FastACos(SampleCos);
                const float NewArc  = XeGTAO_SliceArc(S, (Side == 0u) ? Angle : -Angle);
                const float Covered = max(Arc[Side] - NewArc, 0.0);

                SliceBounce   += Covered * HitRadiance(SampleUV);
                SliceCoverage += Covered;
                HorizonCos[Side] = SampleCos;
                Arc[Side]        = NewArc;
            }
        }

        Bounce   += S.ProjNLen * SliceBounce;
        Coverage += S.ProjNLen * SliceCoverage;
    }

    Pass().Output[PixCoord] = float4(Bounce / float(SliceCount), saturate(Coverage / float(SliceCount)));
}

#else

struct FPushConstants
{
    uint2        OutputSize;
    uint2        WorkingDepthSize;
    FTexture2D   WorkingDepth;
    FTexture2D   Source;
    FRWTexture2D Output;
    uint         DepthScale;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const int kBlurRadius = 2;

float TexelDepth(int2 Texel)
{
    const uint2 DepthCoord = min(uint2(Texel) * Pass().DepthScale, Pass().WorkingDepthSize - 1u);
    return Pass().WorkingDepth.Load(int3(DepthCoord, 0)).r;
}

[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const int2 Pixel = int2(DTid.xy);
    if (any(DTid.xy >= Pass().OutputSize))
    {
        return;
    }

    const float Z         = TexelDepth(Pixel);
    const float InvSigmaZ = 1.0 / (XE_GTAO_DEPTH_SIGMA * Z);
    const int2  MaxTap    = int2(Pass().OutputSize) - 1;

    float4 Sum       = float4(0.0, 0.0, 0.0, 0.0);
    float  SumWeight = 0.0;

    [unroll]
    for (int Y = -kBlurRadius; Y <= kBlurRadius; ++Y)
    {
        [unroll]
        for (int X = -kBlurRadius; X <= kBlurRadius; ++X)
        {
            const int2  Tap    = clamp(Pixel + int2(X, Y), int2(0, 0), MaxTap);
            const float Weight = XeGTAO_DepthWeight(abs(TexelDepth(Tap) - Z), InvSigmaZ);

            Sum       += Pass().Source.Load(int3(Tap, 0)) * Weight;
            SumWeight += Weight;
        }
    }

    Pass().Output[DTid.xy] = Sum / max(SumWeight, 1e-6);
}

#endif
