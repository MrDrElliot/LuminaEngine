#pragma once
#include "SceneGlobals.slang"

// Shared terrain GPU layouts. Keep in sync with the matching structs in TerrainRenderTypes.h.

struct FTerrainRenderParams
{
    float2  OriginXZ;
    float   TileWorldSize;
    float   MaxHeight;

    int     Resolution;
    int     ChunkResolution;
    int     ChunksPerSide;
    int     LayerCount;

    float3  WorldOriginY;
    uint    EntityID;

    uint    MaterialIndex;
    int     MeshletsPerChunkSide;
    int     MeshletQuadSide;
    uint    _Pad0;
};

struct FTerrainChunkInfo
{
    float3 BoundsMin;
    float  _Pad0;
    float3 BoundsMax;
    float  _Pad1;
    int2   ChunkCoord;
    int2   QuadOrigin;
    uint   MeshletOffset;
    uint   MeshletCount;
    uint   _Pad2;
    uint   _Pad3;
};

struct FTerrainMeshletInfo
{
    float3 BoundsMin;
    float  _Pad0;
    float3 BoundsMax;
    float  _Pad1;
    int2   ChunkLocalQuadOrigin;
    int2   QuadExtent;
    uint   ChunkIndex;
    uint   _Pad2;
    uint   _Pad3;
    uint   _Pad4;
};

struct FTerrainVisibleMeshlet
{
    uint ChunkIndex;
    uint MeshletIndex;
};

// One push range shared by the terrain VS and PS, and the PS never dereferences the cull pointers.
struct FTerrainPushConstants
{
    uint64_t                        Params;      // FTerrainRenderParams, see LoadBlock
    GPUSpan<FTerrainChunkInfo>      Chunks;
    GPUSpan<FTerrainMeshletInfo>    Meshlets;
    GPUSpan<FTerrainVisibleMeshlet> Visible;
    FTexture2D                      Heightmap;
    FTexture2D                      Normal;
    FTexture2DArray                 LayerWeights;
    uint                            _Pad0;
};
