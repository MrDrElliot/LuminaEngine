#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// 13-tap partial-overlap downsample (Jimenez, Call of Duty Advanced Warfare); filtering every level keeps bloom stable under motion.

struct FPushConstants
{
    float2     SrcTexelSize;
    FTexture2D Src;
    float      SrcMip;         // mip level to sample on the source SRV

    uint2        DstSize;
    FRWTexture2D Dst;
    uint         bFirstPass;   // 1 prefilters from HDR with Karis, threshold and sanitize

    float      Threshold;    // post-exposure knee, divided by the exposure scale before use
    float      SoftKnee;     // knee width as a fraction of the threshold
    FTexture2D AdaptedLum;   // 1x1 adapted luminance, mirrors ColorGrading
    float      Exposure;     // manual exposure multiplier (2^EV)

    float  AutoExposureKey;    // middle-gray key; <= 0 disables auto-exposure
    float  AutoExposureMinMul; // 2^MinEV clamp on the adapted multiplier
    float  AutoExposureMaxMul; // 2^MaxEV clamp on the adapted multiplier
    float  _Pad;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Mirrors ColorGrading, so a dim scene still blooms its highlights and a bright one does not bloom flat.
float BloomExposureScale()
{
    float AEScale = 1.0;
    if (Pass().AutoExposureKey > 0.0)
    {
        float AdaptedLum = Pass().AdaptedLum.SampleLevel(
            Samplers::LinearClamp, float2(0.5, 0.5), 0.0).r;

        // Frame 0 reads the 1x1 image before the average pass seeds it; a NaN here reaches the chain.
        if (!(AdaptedLum > 1e-6 && AdaptedLum < 1e6))
        {
            AdaptedLum = Pass().AutoExposureKey;
        }

        AEScale = clamp(Pass().AutoExposureKey / max(AdaptedLum, 1e-4),
                        Pass().AutoExposureMinMul, Pass().AutoExposureMaxMul);
    }
    return max(Pass().Exposure * AEScale, 1e-6);
}

float3 SampleSrc(float2 UV, float2 Off)
{
    return Pass().Src.SampleLevel(
        Samplers::LinearClamp, UV + Off * Pass().SrcTexelSize, Pass().SrcMip).rgb;
}

float3 QuadraticThreshold(float3 Color, float Threshold, float3 Curve)
{
    float Br  = max(max(Color.r, Color.g), Color.b);
    float Rq  = clamp(Br - Curve.x, 0.0, Curve.y);
    Rq        = (Rq * Rq) * Curve.z;
    float Mul = max(Rq, Br - Threshold) / max(Br, 1e-4);
    return Color * Mul;
}

// Karis weight, since a lone very bright texel would otherwise alias into a flickering firefly.
float KarisWeight(float3 Color)
{
    float Luma = Luminance(Color) * 0.25;
    return 1.0 / (1.0 + Luma);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    const int2 DstP = int2(DispatchID.xy);
    if (any(uint2(DstP) >= Pass().DstSize))
    {
        return;
    }

    const float2 UV = (float2(DstP) + 0.5) / float2(Pass().DstSize);

    float3 a = SampleSrc(UV, float2(-2.0, -2.0));
    float3 b = SampleSrc(UV, float2( 0.0, -2.0));
    float3 c = SampleSrc(UV, float2( 2.0, -2.0));

    float3 d = SampleSrc(UV, float2(-2.0,  0.0));
    float3 e = SampleSrc(UV, float2( 0.0,  0.0));
    float3 f = SampleSrc(UV, float2( 2.0,  0.0));

    float3 g = SampleSrc(UV, float2(-2.0,  2.0));
    float3 h = SampleSrc(UV, float2( 0.0,  2.0));
    float3 i = SampleSrc(UV, float2( 2.0,  2.0));

    float3 j = SampleSrc(UV, float2(-1.0, -1.0));
    float3 k = SampleSrc(UV, float2( 1.0, -1.0));
    float3 l = SampleSrc(UV, float2(-1.0,  1.0));
    float3 m = SampleSrc(UV, float2( 1.0,  1.0));

    float3 Q0 = (j + k + l + m) * 0.25;
    float3 Q1 = (a + b + d + e) * 0.25;
    float3 Q2 = (b + c + e + f) * 0.25;
    float3 Q3 = (d + e + g + h) * 0.25;
    float3 Q4 = (e + f + h + i) * 0.25;

    float3 Result;
    if (Pass().bFirstPass != 0)
    {
        // A non-finite texel would zero its Karis weight and turn the sum into NaN, which spreads up the chain.
        Q0 = SanitizeRadiance(Q0);
        Q1 = SanitizeRadiance(Q1);
        Q2 = SanitizeRadiance(Q2);
        Q3 = SanitizeRadiance(Q3);
        Q4 = SanitizeRadiance(Q4);

        float W0 = KarisWeight(Q0) * 0.5;
        float W1 = KarisWeight(Q1) * 0.125;
        float W2 = KarisWeight(Q2) * 0.125;
        float W3 = KarisWeight(Q3) * 0.125;
        float W4 = KarisWeight(Q4) * 0.125;

        Result = (Q0 * W0 + Q1 * W1 + Q2 * W2 + Q3 * W3 + Q4 * W4)
               / max(W0 + W1 + W2 + W3 + W4, 1e-4);
        // The chain stays in raw HDR, so ColorGrading's add-then-expose ordering is unaffected.
        float EffThreshold = Pass().Threshold / BloomExposureScale();
        float Knee         = Pass().SoftKnee * EffThreshold + 1e-5;
        float3 KneeCurve   = float3(EffThreshold - Knee, 2.0 * Knee, 0.25 / Knee);

        Result = QuadraticThreshold(max(Result, 0.0), EffThreshold, KneeCurve);
    }
    else
    {
        Result = Q0 * 0.5 + (Q1 + Q2 + Q3 + Q4) * 0.125;
    }

    Pass().Dst[DstP] = float4(Result, 0);
}
