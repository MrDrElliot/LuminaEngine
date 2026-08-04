#pragma once
#include "TerrainCommon.slang"

// Terrain data access + layer sampling + layer blending -- the single home for everything terrain-specific
// a shader (vertex or pixel) or the material codegen needs. The shading itself is NOT here: terrain runs the
// shared ShadeSurface (SurfaceShading.slang), the same path meshes use, so lighting can never drift.
//
// Responsibilities, one per section:
//   - data access : the FTerrainPushConstants accessor both terrain stages bind,
//   - layer sampling : per-pixel weight fetch from the bindless layer-weight array,
//   - layer blending : the weighted-sum helper used by the default terrain material AND the
//                      TerrainLayerBlend material node, so the blend formula lives in exactly one location.

// ---------------------------------------------------------------------------------------------------
// Terrain data access
// ---------------------------------------------------------------------------------------------------

// One push range, identical layout in the terrain VS and PS (see FTerrainPushConstants in TerrainCommon).
FTerrainPushConstants* TerrainArgs() { return GetPassArgs<FTerrainPushConstants>(); }

FTerrainRenderParams GetTerrainParams() { return LoadBlock<FTerrainRenderParams>(TerrainArgs().Params); }

// ---------------------------------------------------------------------------------------------------
// Layer sampling
// ---------------------------------------------------------------------------------------------------

int GetTerrainLayerCount() { return GetTerrainParams().LayerCount; }

float GetTerrainLayerWeight(float2 HeightUV, int LayerIndex)
{
    return gTextures2DArray[TerrainArgs().LayerWeightsIndex]
        .Sample(gSamplers[SAMPLER_LINEAR_CLAMP], float3(HeightUV, float(LayerIndex))).r;
}

// Layer 0 fills any unpainted weight so brushes blend over a base layer; otherwise a tiny edge weight
// would renormalize to 100% and produce hard stroke boundaries.
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

// ---------------------------------------------------------------------------------------------------
// Layer blending
// ---------------------------------------------------------------------------------------------------

// Weighted blend of four per-layer colors by the normalized layer weights at HeightUV. The default terrain
// material and the TerrainLayerBlend node both emit a call to this, so the formula exists in one place.
float3 BlendTerrainLayers4(float3 Layer0, float3 Layer1, float3 Layer2, float3 Layer3, float2 HeightUV)
{
    float4 W = GetTerrainLayerWeights4(HeightUV);
    return Layer0 * W.x + Layer1 * W.y + Layer2 * W.z + Layer3 * W.w;
}
