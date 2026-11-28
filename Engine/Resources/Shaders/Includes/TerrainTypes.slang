#pragma once

// Shared terrain GPU layouts. Keep in sync with the matching structs in TerrainRenderTypes.h.

struct FTerrainRenderParams
{
    float2  OriginXZ;
    float   TileWorldSize;
    float   MaxHeight;

    int     Resolution;
    int     TreeDepth;
    int     LayerCount;
    float   SampleSpacing;

    float3  WorldOriginY;
    uint    EntityID;

    uint    MaterialIndex;
    // Pages sit at depths 0 through this, the deepest holding mip 0.
    int     PageDepth;
    uint    Flags;
    uint    _Pad0;
};

// Mirrors GTerrainFlag* in TerrainRenderTypes.h.
static const uint TERRAIN_FLAG_CAST_SHADOW = 1u;

// The level of the node across each edge sits in four bits apiece, -X, +X, -Y, +Y from the low bits up.
struct FTerrainVisibleNode
{
    uint Node;
    uint NeighborLevels;
};

// Everything one terrain draws and shades from, published per frame in FSceneRoot::Terrains.
struct FTerrainRecord
{
    FTerrainRenderParams            Params;
    // Normalized min and max height under each quadtree node, root first.
    GPUSpan<float2>                 NodeBounds;
    GPUSpan<FTerrainVisibleNode>    Visible;
    // The atlas slot of each page node, TERRAIN_PAGE_NONE while it is not resident.
    GPUSpan<uint>                   PageTable;
    FTexture2D                      HeightAtlas;
    FTexture2D                      NormalAtlas;
    FTexture2D                      WeightAtlas;
    // Where this terrain's visible nodes start in the frame's terrain VisID slots.
    uint                            VisibleBase;
};
