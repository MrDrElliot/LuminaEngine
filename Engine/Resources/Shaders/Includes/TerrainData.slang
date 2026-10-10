#pragma once
#include "TerrainCommon.slang"
#include "Wind.slang"   // material-graph WindAnimation node; TerrainBaseVertexPass needs it for WPO
#include "Triplanar.slang"

// One push range with an identical layout in the terrain VS and PS.
FTerrainPushConstants* TerrainArgs() { return GetPassArgs<FTerrainPushConstants>(); }

FTerrainRenderParams GetTerrainParams() { return LoadBlock<FTerrainRenderParams>(TerrainArgs().Params); }

int GetTerrainLayerCount() { return GetTerrainParams().LayerCount; }

float GetTerrainLayerWeight(float2 HeightUV, int LayerIndex)
{
    return TerrainArgs().LayerWeights
        .Sample(Samplers::LinearClamp, float3(HeightUV, float(LayerIndex))).r;
}

// Layer 0 fills unpainted weight, else a tiny edge weight would renormalize to 100% and leave hard stroke edges.
float4 GetTerrainLayerWeights4(float2 HeightUV)
{
    float4 W = float4(0.0, 0.0, 0.0, 0.0);
    int Count = min(GetTerrainLayerCount(), 4);
    [unroll] for (int i = 0; i < 4; ++i)
    {
        if (i < Count)
        {
            W[i] = GetTerrainLayerWeight(HeightUV, i);
        }
    }
    float Total = W.x + W.y + W.z + W.w;
    if (Total < 1.0)
    {
        W[0] += 1.0 - Total;
        Total = 1.0;
    }
    W /= Total;
    return W;
}

// The default terrain material and the TerrainLayerBlend node both call this, so the formula lives in one place.
float3 BlendTerrainLayers4(float3 Layer0, float3 Layer1, float3 Layer2, float3 Layer3, float2 HeightUV)
{
    float4 W = GetTerrainLayerWeights4(HeightUV);
    return Layer0 * W.x + Layer1 * W.y + Layer2 * W.z + Layer3 * W.w;
}
