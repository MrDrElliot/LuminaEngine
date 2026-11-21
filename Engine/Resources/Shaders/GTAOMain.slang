#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/XeGTAO.slang"
#include "Includes/CORTAO.slang"

// Horizon search over the prefiltered depth pyramid, or a CORTAO trace when CORTAO_SAMPLE_COUNT is defined, writing noisy visibility, its bent normal and packed edges.

struct FPushConstants
{
    uint2        ViewportSize;
    FTexture2D   WorkingDepth;
    FRWTexture2D AOTerm;
    FRWTexture2D Edges;
    uint         SliceCount;
    uint         StepsPerSlice;
    uint         NoiseIndex;
    float        EffectRadius;
    float        EffectFalloffRange;
    float        SampleDistributionPower;
    float        ThinOccluderCompensation;
    float        FinalValuePower;
    float        DepthMIPSamplingOffset;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float SampleWorkingDepth(float2 UV, float MipLevel)
{
    return Pass().WorkingDepth.SampleLevel(Samplers::PointClamp, UV, MipLevel).x;
}

// Total occlusion is never right for a pixel the camera can see, and it wrecks the denoise weights.
static const float kMinVisibility = 0.03;

void StoreVisibility(uint2 PixCoord, float Visibility, float3 BentNormal, float3 Normal)
{
    const float  Packed       = saturate(max(kMinVisibility, Visibility) / XE_GTAO_OCCLUSION_TERM_SCALE);
    const float3 BentOffsetWS = mul((float3x3)GetInverseCameraView(), BentNormal - Normal);
    Pass().AOTerm[PixCoord] = float4(Packed, EncodeBentNormalOffset(BentOffsetWS));
}

[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= Pass().ViewportSize))
    {
        return;
    }

    const FGTAOView   V        = MakeGTAOView(Pass().ViewportSize);
    const uint2       PixCoord = DTid.xy;
    const float2      ScreenUV = (float2(PixCoord) + 0.5) * V.ViewportPixelSize;
    const FGTAOCenter Center   = XeGTAO_LoadCenter(V, Pass().WorkingDepth, PixCoord);

    Pass().Edges[PixCoord] = float4(XeGTAO_PackEdges(Center.EdgesLRTB), 0.0, 0.0, 0.0);

    // Nudge the center toward the camera so depth quantization cannot make a texel occlude itself.
    const float ViewspaceZ = Center.ViewspaceZ * 0.99999;

    const float3 PixCenterPos = XeGTAO_ComputeViewspacePosition(V, ScreenUV, ViewspaceZ);
    const float3 ViewVec      = normalize(-PixCenterPos);

    const float        EffectRadius = Pass().EffectRadius;
    const FGTAOFalloff Falloff      = XeGTAO_MakeFalloff(EffectRadius, Pass().EffectFalloffRange, Pass().ThinOccluderCompensation);

    const float PixelSizeAtZ       = abs(V.NDCToViewMulPixelSize.x * ViewspaceZ);
    const float ScreenspaceRadius  = EffectRadius / max(PixelSizeAtZ, 1e-8);

    // Under a pixel of coverage there is no horizon to find, and MinS below would march off screen.
    if (ScreenspaceRadius < XE_GTAO_PIXEL_TOO_CLOSE)
    {
        StoreVisibility(PixCoord, 1.0, Center.Normal, Center.Normal);
        return;
    }

    const float2 LocalNoise  = XeGTAO_SpatioTemporalNoise(PixCoord, Pass().NoiseIndex);

#ifdef CORTAO_SAMPLE_COUNT
    float3 BentNormal;
    const float Visibility = CORTAO_Visibility(V, Pass().WorkingDepth, XeGTAO_ComputeViewspacePosition(V, ScreenUV, Center.ViewspaceZ),
        Center.Normal, EffectRadius, LocalNoise, CORTAO_Hash(PixCoord, Pass().NoiseIndex), BentNormal);
    StoreVisibility(PixCoord, Visibility, BentNormal, Center.Normal);
#else
    const float  NoiseSlice  = LocalNoise.x;
    const float  NoiseSample = LocalNoise.y;

    const float MinS = XE_GTAO_PIXEL_TOO_CLOSE / ScreenspaceRadius;

    const uint SliceCount    = max(Pass().SliceCount, 1u);
    const uint StepsPerSlice = max(Pass().StepsPerSlice, 1u);

    // Fades the effect out where the footprint is only a few pixels wide, matching the reference.
    float Visibility = saturate((10.0 - ScreenspaceRadius) / 100.0) * 0.5;
    float3 BentNormal = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint Slice = 0; Slice < SliceCount; ++Slice)
    {
        const float      SliceK = (float(Slice) + NoiseSlice) / float(SliceCount);
        const FGTAOSlice S      = XeGTAO_MakeSlice(V, SliceK * XE_GTAO_PI, ScreenspaceRadius, ViewVec, Center.Normal);

        float HorizonCos0 = S.LowHorizonCos0;
        float HorizonCos1 = S.LowHorizonCos1;

        [loop]
        for (uint Step = 0; Step < StepsPerSlice; ++Step)
        {
            const float Distance = XeGTAO_StepDistance(Slice, Step, StepsPerSlice, NoiseSample, Pass().SampleDistributionPower, MinS);

            float MipLevel;
            const float2 SampleOffset = XeGTAO_StepOffsetUV(V, S, Distance, Pass().DepthMIPSamplingOffset, MipLevel);

            const float2 SampleUV0 = ScreenUV + SampleOffset;
            const float2 SampleUV1 = ScreenUV - SampleOffset;

            const float3 SamplePos0 = XeGTAO_ComputeViewspacePosition(V, SampleUV0, SampleWorkingDepth(SampleUV0, MipLevel));
            const float3 SamplePos1 = XeGTAO_ComputeViewspacePosition(V, SampleUV1, SampleWorkingDepth(SampleUV1, MipLevel));

            HorizonCos0 = max(HorizonCos0, XeGTAO_SampleHorizonCos(Falloff, SamplePos0 - PixCenterPos, ViewVec, S.LowHorizonCos0));
            HorizonCos1 = max(HorizonCos1, XeGTAO_SampleHorizonCos(Falloff, SamplePos1 - PixCenterPos, ViewVec, S.LowHorizonCos1));
        }

        // Fudge from the reference against slight over-darkening on steep slopes.
        const float ProjNLen = lerp(S.ProjNLen, 1.0, 0.05);

        const float H0 = -XeGTAO_FastACos(HorizonCos1);
        const float H1 =  XeGTAO_FastACos(HorizonCos0);

        Visibility += ProjNLen * (XeGTAO_SliceArc(S, H0) + XeGTAO_SliceArc(S, H1));
        BentNormal += XeGTAO_SliceBentNormal(S, H0, H1, ViewVec);
    }

    Visibility /= float(SliceCount);
    Visibility = pow(Visibility, Pass().FinalValuePower);

    const float BentLength = length(BentNormal);
    StoreVisibility(PixCoord, Visibility, BentLength > 1e-6 ? BentNormal / BentLength : Center.Normal, Center.Normal);
#endif
}
