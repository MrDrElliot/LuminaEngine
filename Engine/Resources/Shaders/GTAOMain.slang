#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/XeGTAO.slang"

// Horizon search over the prefiltered depth pyramid; writes the noisy visibility term plus packed edges.

struct FPushConstants
{
    uint2 ViewportSize;
    uint  WorkingDepthIndex;
    uint  AOTermUAV;
    uint  EdgesUAV;
    uint  SliceCount;
    uint  StepsPerSlice;
    uint  NoiseIndex;
    float EffectRadius;
    float EffectFalloffRange;
    float SampleDistributionPower;
    float ThinOccluderCompensation;
    float FinalValuePower;
    float DepthMIPSamplingOffset;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float SampleWorkingDepth(float2 UV, float MipLevel)
{
    return gTextures2D[Pass().WorkingDepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, MipLevel).x;
}

void StoreVisibility(uint2 PixCoord, float Visibility)
{
    const float Packed = saturate(Visibility / XE_GTAO_OCCLUSION_TERM_SCALE);
    gRWTextures2D[Pass().AOTermUAV][PixCoord] = float4(Packed, 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= Pass().ViewportSize))
    {
        return;
    }

    const FGTAOView V        = MakeGTAOView(Pass().ViewportSize);
    const uint2     PixCoord = DTid.xy;
    const float2    ScreenUV = (float2(PixCoord) + 0.5) * V.ViewportPixelSize;

    // Mip 0 of the working pyramid is the unfiltered linearized depth, so the cross reads it directly.
    const float2 GatherUV = float2(PixCoord) * V.ViewportPixelSize;
    const float4 ValuesUL = gTextures2D[Pass().WorkingDepthIndex].GatherRed(gSamplers[SAMPLER_POINT_CLAMP], GatherUV);
    const float4 ValuesBR = gTextures2D[Pass().WorkingDepthIndex].GatherRed(gSamplers[SAMPLER_POINT_CLAMP], GatherUV, int2(1, 1));

    float       ViewspaceZ = ValuesUL.y;
    const float PixLZ      = ValuesUL.x;
    const float PixTZ      = ValuesUL.z;
    const float PixRZ      = ValuesBR.z;
    const float PixBZ      = ValuesBR.x;

    const float4 EdgesLRTB = XeGTAO_CalculateEdges(ViewspaceZ, PixLZ, PixRZ, PixTZ, PixBZ);
    gRWTextures2D[Pass().EdgesUAV][PixCoord] = float4(XeGTAO_PackEdges(EdgesLRTB), 0.0, 0.0, 0.0);

    const float3 CenterPos = XeGTAO_ComputeViewspacePosition(V, ScreenUV, ViewspaceZ);
    const float3 LeftPos   = XeGTAO_ComputeViewspacePosition(V, ScreenUV + float2(-1.0,  0.0) * V.ViewportPixelSize, PixLZ);
    const float3 RightPos  = XeGTAO_ComputeViewspacePosition(V, ScreenUV + float2( 1.0,  0.0) * V.ViewportPixelSize, PixRZ);
    const float3 TopPos    = XeGTAO_ComputeViewspacePosition(V, ScreenUV + float2( 0.0, -1.0) * V.ViewportPixelSize, PixTZ);
    const float3 BottomPos = XeGTAO_ComputeViewspacePosition(V, ScreenUV + float2( 0.0,  1.0) * V.ViewportPixelSize, PixBZ);

    float3 ViewspaceNormal = XeGTAO_CalculateNormal(EdgesLRTB, CenterPos, LeftPos, RightPos, TopPos, BottomPos);

    // Nudge the center toward the camera so depth quantization cannot make a texel occlude itself.
    ViewspaceZ *= 0.99999;

    const float3 PixCenterPos = XeGTAO_ComputeViewspacePosition(V, ScreenUV, ViewspaceZ);
    const float3 ViewVec      = normalize(-PixCenterPos);

    // The cross-product winding depends on the projection's handedness, so resolve it against the view.
    if (dot(ViewspaceNormal, ViewVec) < 0.0)
    {
        ViewspaceNormal = -ViewspaceNormal;
    }

    const float EffectRadius             = Pass().EffectRadius;
    const float EffectFalloffRange       = Pass().EffectFalloffRange;
    const float SampleDistributionPower  = Pass().SampleDistributionPower;
    const float ThinOccluderCompensation = Pass().ThinOccluderCompensation;

    const float FalloffRange = max(EffectFalloffRange * EffectRadius, 1e-6);
    const float FalloffFrom  = EffectRadius * (1.0 - EffectFalloffRange);
    const float FalloffMul   = -1.0 / FalloffRange;
    const float FalloffAdd   = FalloffFrom / FalloffRange + 1.0;

    const float PixelSizeAtZ       = abs(V.NDCToViewMulPixelSize.x * ViewspaceZ);
    const float ScreenspaceRadius  = EffectRadius / max(PixelSizeAtZ, 1e-8);

    // Under a pixel of coverage there is no horizon to find, and MinS below would march off screen.
    if (ScreenspaceRadius < XE_GTAO_PIXEL_TOO_CLOSE)
    {
        StoreVisibility(PixCoord, 1.0);
        return;
    }

    // The reference bakes the Y flip into its screen march; derive it so either handedness works.
    const float SignX = (V.NDCToViewMulPixelSize.x < 0.0) ? -1.0 : 1.0;
    const float SignY = (V.NDCToViewMulPixelSize.y < 0.0) ? -1.0 : 1.0;

    const float2 LocalNoise  = XeGTAO_SpatioTemporalNoise(PixCoord, Pass().NoiseIndex);
    const float  NoiseSlice  = LocalNoise.x;
    const float  NoiseSample = LocalNoise.y;

    const float MinS = XE_GTAO_PIXEL_TOO_CLOSE / ScreenspaceRadius;

    const uint SliceCount    = max(Pass().SliceCount, 1u);
    const uint StepsPerSlice = max(Pass().StepsPerSlice, 1u);

    // Fades the effect out where the footprint is only a few pixels wide, matching the reference.
    float Visibility = saturate((10.0 - ScreenspaceRadius) / 100.0) * 0.5;

    [loop]
    for (uint Slice = 0; Slice < SliceCount; ++Slice)
    {
        const float SliceK = (float(Slice) + NoiseSlice) / float(SliceCount);
        const float Phi    = SliceK * XE_GTAO_PI;
        const float CosPhi = cos(Phi);
        const float SinPhi = sin(Phi);

        const float2 Omega = float2(CosPhi * SignX, SinPhi * SignY) * ScreenspaceRadius;

        const float3 DirectionVec      = float3(CosPhi, SinPhi, 0.0);
        const float3 OrthoDirectionVec = DirectionVec - (dot(DirectionVec, ViewVec) * ViewVec);
        const float3 AxisVec           = normalize(cross(OrthoDirectionVec, ViewVec));

        const float3 ProjectedNormalVec = ViewspaceNormal - AxisVec * dot(ViewspaceNormal, AxisVec);

        const float SignNorm  = sign(dot(OrthoDirectionVec, ProjectedNormalVec));
        float       ProjNLen  = length(ProjectedNormalVec);
        const float CosNorm   = saturate(dot(ProjectedNormalVec, ViewVec) / max(ProjNLen, 1e-6));
        const float SliceN    = SignNorm * XeGTAO_FastACos(CosNorm);

        // Below the horizon a sample is not "unoccluded", so the reference seeds from the tangent plane.
        const float LowHorizonCos0 = cos(SliceN + XE_GTAO_PI_HALF);
        const float LowHorizonCos1 = cos(SliceN - XE_GTAO_PI_HALF);

        float HorizonCos0 = LowHorizonCos0;
        float HorizonCos1 = LowHorizonCos1;

        [loop]
        for (uint Step = 0; Step < StepsPerSlice; ++Step)
        {
            const float StepBaseNoise = float(Slice + Step * StepsPerSlice) * 0.6180339887498948482;
            const float StepNoise     = frac(NoiseSample + StepBaseNoise);

            float S = (float(Step) + StepNoise) / float(StepsPerSlice);
            S = pow(S, SampleDistributionPower);
            S += MinS;

            float2      SampleOffset       = S * Omega;
            const float SampleOffsetLength = length(SampleOffset);

            // Stepping the mip with the offset is what keeps a wide radius off the full-res cache.
            const float MipLevel = clamp(log2(SampleOffsetLength) - Pass().DepthMIPSamplingOffset,
                                         0.0, float(XE_GTAO_DEPTH_MIP_LEVELS));

            SampleOffset = round(SampleOffset) * V.ViewportPixelSize;

            const float2 SampleUV0 = ScreenUV + SampleOffset;
            const float2 SampleUV1 = ScreenUV - SampleOffset;

            const float3 SamplePos0 = XeGTAO_ComputeViewspacePosition(V, SampleUV0, SampleWorkingDepth(SampleUV0, MipLevel));
            const float3 SamplePos1 = XeGTAO_ComputeViewspacePosition(V, SampleUV1, SampleWorkingDepth(SampleUV1, MipLevel));

            const float3 Delta0 = SamplePos0 - PixCenterPos;
            const float3 Delta1 = SamplePos1 - PixCenterPos;
            const float  Dist0  = max(length(Delta0), 1e-6);
            const float  Dist1  = max(length(Delta1), 1e-6);

            // Stretching the depth axis discards samples behind the center sooner, which is the thickness heuristic.
            const float FalloffBase0 = length(float3(Delta0.x, Delta0.y, Delta0.z * (1.0 + ThinOccluderCompensation)));
            const float FalloffBase1 = length(float3(Delta1.x, Delta1.y, Delta1.z * (1.0 + ThinOccluderCompensation)));

            const float Weight0 = saturate(FalloffBase0 * FalloffMul + FalloffAdd);
            const float Weight1 = saturate(FalloffBase1 * FalloffMul + FalloffAdd);

            const float SHC0 = lerp(LowHorizonCos0, dot(Delta0 / Dist0, ViewVec), Weight0);
            const float SHC1 = lerp(LowHorizonCos1, dot(Delta1 / Dist1, ViewVec), Weight1);

            HorizonCos0 = max(HorizonCos0, SHC0);
            HorizonCos1 = max(HorizonCos1, SHC1);
        }

        // Fudge from the reference against slight over-darkening on steep slopes.
        ProjNLen = lerp(ProjNLen, 1.0, 0.05);

        const float H0 = -XeGTAO_FastACos(HorizonCos1);
        const float H1 =  XeGTAO_FastACos(HorizonCos0);

        const float SinSliceN = sin(SliceN);
        const float IArc0 = (CosNorm + 2.0 * H0 * SinSliceN - cos(2.0 * H0 - SliceN)) / 4.0;
        const float IArc1 = (CosNorm + 2.0 * H1 * SinSliceN - cos(2.0 * H1 - SliceN)) / 4.0;

        Visibility += ProjNLen * (IArc0 + IArc1);
    }

    Visibility /= float(SliceCount);
    Visibility = pow(Visibility, Pass().FinalValuePower);

    // Total occlusion is never right for a pixel the camera can see, and it wrecks the denoise weights.
    Visibility = max(0.03, Visibility);

    StoreVisibility(PixCoord, Visibility);
}
