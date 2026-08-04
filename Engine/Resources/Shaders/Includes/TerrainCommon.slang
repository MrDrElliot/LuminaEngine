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

// Shared by the terrain VS and PS (one push range, identical layout in both stages). Cull buffers are
// reached by device pointer (VS walks them); textures by bindless index. The PS only reads Params and
// LayerWeightsIndex -- it never dereferences the cull pointers.
struct FTerrainPushConstants
{
    uint64_t Params;      // FTerrainRenderParams, see LoadBlock
    FTerrainChunkInfo*      Chunks;
    FTerrainMeshletInfo*    Meshlets;
    FTerrainVisibleMeshlet* Visible;
    uint HeightmapIndex;       // bindless 2D
    uint NormalIndex;          // bindless 2D
    uint LayerWeightsIndex;    // bindless 2D-array
    uint _Pad0;
};
