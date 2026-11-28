#pragma once
#include "TerrainCommon.slang"
#include "Wind.slang"   // material-graph WindAnimation node; the terrain templates need it for WPO
#include "Triplanar.slang"

int GetTerrainLayerCount() { return GetTerrainParams().LayerCount; }

// The mip shading reads the heights and weights at, which a deferred entry point sets from the pixel's footprint and a raster one from its node.
static float GTerrainShadingMip = 0.0;

float2 TerrainHeightUVToSample(float2 HeightUV)
{
    return HeightUV * float(GetTerrainParams().Resolution) - 0.5;
}

// A pixel reads its normal and every layer weight at one spot and mip, so the page pair and the weights resolve once per invocation.
struct FTerrainShadingCache
{
    float2          HeightUV;
    float           Mip;
    FTerrainMipPair Pair;
    float4          Weights;
    bool            bPair;
    bool            bWeights;
};
static FTerrainShadingCache GTerrainShading = (FTerrainShadingCache)0;

FTerrainMipPair TerrainShadingPair(float2 HeightUV)
{
    if (!GTerrainShading.bPair || any(GTerrainShading.HeightUV != HeightUV) || GTerrainShading.Mip != GTerrainShadingMip)
    {
        GTerrainShading.HeightUV = HeightUV;
        GTerrainShading.Mip      = GTerrainShadingMip;
        GTerrainShading.Pair     = TerrainFindMipPair(TerrainArgs(), TerrainHeightUVToSample(HeightUV), GTerrainShadingMip);
        GTerrainShading.bPair    = true;
        GTerrainShading.bWeights = false;
    }
    return GTerrainShading.Pair;
}

float3 TerrainSampleNormalAt(float2 HeightUV)
{
    return TerrainPairNormal(TerrainArgs(), TerrainShadingPair(HeightUV), TerrainHeightUVToSample(HeightUV));
}

float4 TerrainSampleWeightsAt(float2 HeightUV)
{
    const FTerrainMipPair Pair = TerrainShadingPair(HeightUV);
    if (!GTerrainShading.bWeights)
    {
        GTerrainShading.Weights  = TerrainPairWeights(TerrainArgs(), Pair, TerrainHeightUVToSample(HeightUV));
        GTerrainShading.bWeights = true;
    }
    return GTerrainShading.Weights;
}

// A border vertex the coarser neighbor across its edge skips moves onto that neighbor's edge, so the two meet without a crack.
void TerrainSnapToNeighbor(uint NeighborLevel, uint Level, int2 Sample, int2 Axis, FTerrainRenderParams Params, inout FTerrainPage Cache,
                           inout float Height, inout float3 Normal)
{
    if (NeighborLevel <= Level)
    {
        return;
    }
    const int Span = 1 << NeighborLevel;
    const int T    = Sample.x * Axis.x + Sample.y * Axis.y;
    const int T0   = T & ~(Span - 1);
    const int T1   = min(T0 + Span, Params.Resolution - 1);
    if (T == T0 || T1 <= T0)
    {
        return;
    }
    const int2  Base = Sample - Axis * T;
    const float F    = float(T - T0) / float(T1 - T0);
    FTerrainRecord* Record = TerrainArgs();
    Height = lerp(TerrainHeightAtSample(Record, Base + Axis * T0, Level, Cache), TerrainHeightAtSample(Record, Base + Axis * T1, Level, Cache), F);
    Normal = normalize(lerp(TerrainNormalAtSample(Record, Base + Axis * T0, Level, Cache), TerrainNormalAtSample(Record, Base + Axis * T1, Level, Cache), F));
}

// Layer 0 fills unpainted weight, else a tiny edge weight would renormalize to 100% and leave hard stroke edges.
float4 GetTerrainLayerWeights4(float2 HeightUV)
{
    const float4 Painted = TerrainSampleWeightsAt(HeightUV);
    float4 W = float4(0.0, 0.0, 0.0, 0.0);
    int Count = min(GetTerrainLayerCount(), 4);
    [unroll] for (int i = 0; i < 4; ++i)
    {
        if (i < Count)
        {
            W[i] = Painted[i];
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

struct FTerrainVertex
{
    float3 WorldPos;
    float3 NormalWS;
    float2 HeightUV;
};

// Where one grid vertex of a visible node lands, shared by the raster and the deferred rebuild so both place it alike. A node lies in one page, so its vertices share the cache.
FTerrainVertex TerrainNodeVertex(FTerrainRenderParams Params, FTerrainVisibleNode Vis, uint GridIndex, inout FTerrainPage Cache)
{
    const FTerrainNode Node       = UnpackTerrainNode(Vis.Node);
    const uint         Level      = TerrainNodeLevel(Params, Node.Depth);
    const int2         GridVertex = int2(GridIndex % TERRAIN_MESHLET_VERTS_PER_SIDE, GridIndex / TERRAIN_MESHLET_VERTS_PER_SIDE);

    // A map that is not a power of two plus one ends inside its last nodes, whose extra quads collapse onto the edge.
    const int  Last     = Params.Resolution - 1;
    const int2 SampleXY = min(Node.Coord * TerrainNodeSpan(Level) + GridVertex * (1 << Level), int2(Last, Last));
    FTerrainRecord* Record = TerrainArgs();
    float  Height   = TerrainHeightAtSample(Record, SampleXY, Level, Cache);
    float3 NormalWS = TerrainNormalAtSample(Record, SampleXY, Level, Cache);

    const int EdgeVertex = int(TERRAIN_MESHLET_QUADS);
    if (GridVertex.x == 0 || GridVertex.x == EdgeVertex)
    {
        const uint NeighborLevel = (Vis.NeighborLevels >> (GridVertex.x == 0 ? 0u : 4u)) & 0xFu;
        TerrainSnapToNeighbor(NeighborLevel, Level, SampleXY, int2(0, 1), Params, Cache, Height, NormalWS);
    }
    if (GridVertex.y == 0 || GridVertex.y == EdgeVertex)
    {
        const uint NeighborLevel = (Vis.NeighborLevels >> (GridVertex.y == 0 ? 8u : 12u)) & 0xFu;
        TerrainSnapToNeighbor(NeighborLevel, Level, SampleXY, int2(1, 0), Params, Cache, Height, NormalWS);
    }

    const float Stride = Params.SampleSpacing;

    FTerrainVertex Out;
    Out.WorldPos = float3(Params.OriginXZ.x + float(SampleXY.x) * Stride,
                          Params.WorldOriginY.x + Height,
                          Params.OriginXZ.y + float(SampleXY.y) * Stride);
    Out.NormalWS = NormalWS;
    Out.HeightUV = (float2(SampleXY) + 0.5) / float(Params.Resolution);
    return Out;
}

// A smooth analytic tangent, since UV is world XZ, which avoids the faceting of a per-triangle one.
float3 TerrainTangent(float3 NormalWS)
{
    const float DhDx = -NormalWS.x / max(NormalWS.y, 1e-4);
    return normalize(float3(1.0, DhDx, 0.0));
}

// Quad-major with two triangles per quad, the order the vertex path's index buffer uses, so a triangle index means the same in every pass.
uint3 TerrainTriangleCorners(uint Tri)
{
    const uint Quad = Tri >> 1u;
    const uint V00  = (Quad / TERRAIN_MESHLET_QUADS) * TERRAIN_MESHLET_VERTS_PER_SIDE + (Quad % TERRAIN_MESHLET_QUADS);
    const uint V01  = V00 + TERRAIN_MESHLET_VERTS_PER_SIDE;
    return (Tri & 1u) == 0u ? uint3(V00, V00 + 1u, V01 + 1u) : uint3(V00, V01 + 1u, V01);
}

// A terrain VisID packs its slot in the frame's terrain node space, which this walks back to the terrain that owns it.
struct FTerrainVisTriangle
{
    FTerrainVisibleNode Node;
    uint                Tri;
    bool                bValid;
};

bool IsVisTerrain(uint VisRaw)
{
    return (VisRaw & VISBUFFER_FLAG_MASK) == VISBUFFER_FLAG_MASK;
}

uint PackTerrainVisID(uint Slot, uint Tri)
{
    return VISBUFFER_FLAG_MASK | ((((Slot << 7u) | (Tri & 0x7Fu)) + 1u) & ~VISBUFFER_FLAG_MASK);
}

// Selects the terrain for the rest of the invocation, so every TerrainArgs read after it reads the pixel's own terrain.
FTerrainVisTriangle ResolveTerrainVisTriangle(uint VisRaw)
{
    FTerrainVisTriangle Out = (FTerrainVisTriangle)0;
    if (!IsVisTerrain(VisRaw))
    {
        return Out;
    }

    const uint Packed = (VisRaw & ~VISBUFFER_FLAG_MASK) - 1u;
    const uint Slot   = Packed >> 7u;
    const GPUSpan<FTerrainRecord> Terrains = GetSceneRoot().Terrains;
    for (uint t = 0u; t < Terrains.Count; ++t)
    {
        FTerrainRecord* Record = Terrains.At(t);
        const uint Base = Record.VisibleBase;
        if (Slot < Base || Slot - Base >= Record.Visible.Count)
        {
            continue;
        }

        GTerrainIndex = t;
        Out.Node   = Record.Visible.Get(Slot - Base);
        Out.Tri    = min(Packed & 0x7Fu, TERRAIN_MESHLET_MAX_TRIANGLES - 1u);
        Out.bValid = true;
        return Out;
    }
    return Out;
}

// The default terrain material and the TerrainLayerBlend node both call this, so the formula lives in one place.
float3 BlendTerrainLayers4(float3 Layer0, float3 Layer1, float3 Layer2, float3 Layer3, float2 HeightUV)
{
    float4 W = GetTerrainLayerWeights4(HeightUV);
    return Layer0 * W.x + Layer1 * W.y + Layer2 * W.z + Layer3 * W.w;
}
