#pragma once

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
    uint   Level;
    uint   _Pad3;
    uint   _Pad4;
};

struct FTerrainVisibleMeshlet
{
    uint ChunkIndex;
    uint MeshletIndex;
};

// Everything one terrain draws and shades from, published per frame in FSceneRoot::Terrains.
struct FTerrainRecord
{
    FTerrainRenderParams            Params;
    GPUSpan<FTerrainChunkInfo>      Chunks;
    GPUSpan<FTerrainMeshletInfo>    Meshlets;
    GPUSpan<FTerrainVisibleMeshlet> Visible;
    GPUSpan<uint>                   ChunkLevels;
    FTexture2D                      Heightmap;
    FTexture2D                      Normal;
    FTexture2DArray                 LayerWeights;
    // Where this terrain's visible meshlets start in the frame's terrain VisID slots.
    uint                            VisibleBase;
};
