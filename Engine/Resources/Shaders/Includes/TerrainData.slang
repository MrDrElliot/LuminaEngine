#pragma once
#include "TerrainCommon.slang"
#include "Wind.slang"   // material-graph WindAnimation node; the terrain templates need it for WPO
#include "Triplanar.slang"

// The terrain a shader invocation is drawing or shading, set by its entry point before any terrain read.
static uint GTerrainIndex = 0u;

FTerrainRecord* TerrainArgs() { return GetSceneRoot().Terrains.At(min(GTerrainIndex, max(GetSceneRoot().Terrains.Count, 1u) - 1u)); }

FTerrainRenderParams GetTerrainParams() { return TerrainArgs().Params; }

int GetTerrainLayerCount() { return GetTerrainParams().LayerCount; }

float TerrainSampleHeight(int2 Sample, FTerrainRenderParams Params)
{
    const float2 UV = (float2(Sample) + 0.5) / float(Params.Resolution);
    return TerrainArgs().Heightmap.SampleLevel(Samplers::LinearClamp, UV, 0).r * Params.MaxHeight;
}

// The compute pass packs Normal.xz, so Y comes back from unit length; a fixed Y of 1 would flatten slopes.
float3 TerrainSampleNormalAt(float2 HeightUV)
{
    const float4 Packed = TerrainArgs().Normal.SampleLevel(Samplers::LinearClamp, HeightUV, 0);
    const float  Nx = Packed.r * 2.0 - 1.0;
    const float  Nz = Packed.g * 2.0 - 1.0;
    return float3(Nx, sqrt(saturate(1.0 - Nx * Nx - Nz * Nz)), Nz);
}

float3 TerrainSampleNormal(int2 Sample, FTerrainRenderParams Params)
{
    return TerrainSampleNormalAt((float2(Sample) + 0.5) / float(Params.Resolution));
}

uint TerrainChunkLevel(int2 ChunkCoord, int ChunksPerSide)
{
    const uint Index = (uint)(ChunkCoord.y * ChunksPerSide + ChunkCoord.x);
    return TerrainArgs().ChunkLevels.Contains(Index) ? TerrainArgs().ChunkLevels.Get(Index) : 0u;
}

// Blends the two vertices of the neighbor's coarser edge around T, along Axis, when the neighbor draws coarser than Level.
void TerrainSnapToNeighbor(int2 Neighbor, int ChunksPerSide, uint Level, int T, int EdgeLength, int2 QuadOrigin, int2 Axis, int2 Local,
                           FTerrainRenderParams Params, inout float Height, inout float3 Normal)
{
    const uint NeighborLevel = TerrainChunkLevel(Neighbor, ChunksPerSide);
    if (NeighborLevel <= Level)
    {
        return;
    }
    const int Span = 1 << NeighborLevel;
    const int T0 = (T / Span) * Span;
    const int T1 = min(T0 + Span, EdgeLength);
    if (T == T0 || T1 <= T0)
    {
        return;
    }
    const int2  Base = QuadOrigin + Local - Axis * T;
    const float F    = float(T - T0) / float(T1 - T0);
    Height = lerp(TerrainSampleHeight(Base + Axis * T0, Params), TerrainSampleHeight(Base + Axis * T1, Params), F);
    Normal = normalize(lerp(TerrainSampleNormal(Base + Axis * T0, Params), TerrainSampleNormal(Base + Axis * T1, Params), F));
}

// The top level, since terrain shades in a compute lane with no implicit derivatives and the weights are already one texel per height sample.
float GetTerrainLayerWeight(float2 HeightUV, int LayerIndex)
{
    return TerrainArgs().LayerWeights
        .SampleLevel(Samplers::LinearClamp, float3(HeightUV, float(LayerIndex)), 0).r;
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

struct FTerrainVertex
{
    float3 WorldPos;
    float3 NormalWS;
    float2 HeightUV;
};

// Where one grid vertex of a visible meshlet lands, shared by the raster and the deferred rebuild so both place it alike.
FTerrainVertex TerrainMeshletVertex(FTerrainRenderParams Params, FTerrainMeshletInfo Meshlet, FTerrainChunkInfo Chunk, uint GridIndex)
{
    const int2 GridVertex = int2(GridIndex % TERRAIN_MESHLET_VERTS_PER_SIDE, GridIndex / TERRAIN_MESHLET_VERTS_PER_SIDE);

    // Edge meshlets cover fewer quads, so the clamp collapses the extras instead of sampling outside the chunk's heightmap region.
    const int2 Clamped = min(GridVertex, max(Meshlet.QuadExtent, int2(1, 1)));

    // Each quad of a coarser level spans 1 << Level samples, the last one clamped to the chunk edge.
    const int  Step          = 1 << Meshlet.Level;
    const int  QuadsPerChunk = Params.ChunkResolution - 1;
    const int2 Extent        = TerrainChunkExtent(Chunk.QuadOrigin, QuadsPerChunk, Params.Resolution);
    int2 Local = min((Meshlet.ChunkLocalQuadOrigin + Clamped) * Step, Extent);

    const int2 SampleXY = clamp(Chunk.QuadOrigin + Local, int2(0, 0), int2(Params.Resolution - 1, Params.Resolution - 1));
    float  Height   = TerrainSampleHeight(SampleXY, Params);
    float3 NormalWS = TerrainSampleNormal(SampleXY, Params);

    // A border vertex a coarser neighbor skips is put on that neighbor's edge, so the two meet without a crack.
    const int2 ChunkCoord    = Chunk.ChunkCoord;
    const int  ChunksPerSide = Params.ChunksPerSide;
    const bool bOnX = (Local.x == 0 && ChunkCoord.x > 0) || (Local.x == Extent.x && ChunkCoord.x + 1 < ChunksPerSide);
    const bool bOnY = (Local.y == 0 && ChunkCoord.y > 0) || (Local.y == Extent.y && ChunkCoord.y + 1 < ChunksPerSide);
    if (bOnX)
    {
        const int2 Neighbor = ChunkCoord + int2(Local.x == 0 ? -1 : 1, 0);
        TerrainSnapToNeighbor(Neighbor, ChunksPerSide, Meshlet.Level, Local.y, Extent.y, Chunk.QuadOrigin, int2(0, 1), Local, Params, Height, NormalWS);
    }
    if (bOnY)
    {
        const int2 Neighbor = ChunkCoord + int2(0, Local.y == 0 ? -1 : 1);
        TerrainSnapToNeighbor(Neighbor, ChunksPerSide, Meshlet.Level, Local.x, Extent.x, Chunk.QuadOrigin, int2(1, 0), Local, Params, Height, NormalWS);
    }

    const float Stride = Params.TileWorldSize / float(Params.Resolution - 1);

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

// A terrain VisID packs its slot in the frame's terrain meshlet space, which this walks back to the terrain that owns it.
struct FTerrainVisTriangle
{
    FTerrainMeshletInfo Meshlet;
    FTerrainChunkInfo   Chunk;
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
        const FTerrainVisibleMeshlet Vis = Record.Visible.Get(Slot - Base);
        if (!Record.Meshlets.Contains(Vis.MeshletIndex))
        {
            return Out;
        }
        Out.Meshlet = Record.Meshlets.Get(Vis.MeshletIndex);
        if (!Record.Chunks.Contains(Out.Meshlet.ChunkIndex))
        {
            return Out;
        }
        Out.Chunk  = Record.Chunks.Get(Out.Meshlet.ChunkIndex);
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
