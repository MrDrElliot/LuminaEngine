#pragma once
#include "Includes/SceneGlobals.slang"

// SMAA T2x resolve. Two jittered subsamples averaged, not an exponential history, so ghosting lasts one frame.

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

struct FData
{
    uint  CurrentIndex;
    uint  HistoryIndex;
    uint  VelocityIndex;
    float HistoryWeight;
    float ClampGamma;
    float MaxReprojection;
    uint  bNeighborhoodClamp;
    uint  _Pad0;
};

float3 SampleOffset(uint Index, float2 UV, float2 Offset)
{
    return gTextures2D[Index].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV + Offset, 0.0).rgb;
}

// Variance clipping; a history the motion vectors aimed wrong falls outside the local range and dies here.
void NeighborhoodBounds(uint Index, float2 UV, float2 TexelSize, float Gamma,
                        out float3 OutMin, out float3 OutMax)
{
    float3 Sum   = float3(0.0, 0.0, 0.0);
    float3 SumSq = float3(0.0, 0.0, 0.0);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 C = SampleOffset(Index, UV, float2(x, y) * TexelSize);
            Sum   += C;
            SumSq += C * C;
        }
    }

    const float3 Mean  = Sum / 9.0;
    const float3 Sigma = sqrt(max(SumSq / 9.0 - Mean * Mean, float3(0.0, 0.0, 0.0)));

    OutMin = Mean - Gamma * Sigma;
    OutMax = Mean + Gamma * Sigma;
}

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    FData* Data = GetPassArgs<FData>();

    const float4 Current = gTextures2D[Data.CurrentIndex]
        .SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], Input.UV, 0.0);

    if (Data.HistoryWeight <= 0.0)
    {
        return Current;
    }

    const float2 Velocity = gTextures2D[Data.VelocityIndex]
        .SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], Input.UV, 0.0).rg;

    // A slot recycled onto a new instance reprojects nonsense, so refuse implausible motion outright.
    if (any(abs(Velocity) > Data.MaxReprojection))
    {
        return Current;
    }

    const float2 HistoryUV = Input.UV + Velocity;

    // Reprojected off screen, so there is no history to blend and the current sample stands alone.
    if (any(saturate(HistoryUV) != HistoryUV))
    {
        return Current;
    }

    float3 History = gTextures2D[Data.HistoryIndex]
        .SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], HistoryUV, 0.0).rgb;

    if (Data.bNeighborhoodClamp != 0u)
    {
        const float2 TexelSize = 1.0 / float2(Scene().ScreenSize.xy);

        float3 BoundsMin;
        float3 BoundsMax;
        NeighborhoodBounds(Data.CurrentIndex, Input.UV, TexelSize, Data.ClampGamma, BoundsMin, BoundsMax);

        History = clamp(History, BoundsMin, BoundsMax);
    }

    return float4(lerp(Current.rgb, History, Data.HistoryWeight), Current.a);
}
