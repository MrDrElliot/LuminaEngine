#pragma once
#include "SceneGlobals.slang"

// Mirrors TerrainRenderTypes.h.
static const uint kTerrainLODLevels    = 6u;

int2 TerrainChunkExtent(int2 QuadOrigin, int QuadsPerChunk, int Resolution)
{
    return min(int2(QuadsPerChunk, QuadsPerChunk), int2(Resolution - 1, Resolution - 1) - QuadOrigin);
}

int2 TerrainLevelQuads(int2 FineQuads, uint Level)
{
    return (FineQuads + int((1u << Level) - 1u)) >> Level;
}

int2 TerrainLevelMeshletsSide(int2 FineQuads, uint Level)
{
    return (TerrainLevelQuads(FineQuads, Level) + int(TERRAIN_MESHLET_QUADS - 1u)) / int(TERRAIN_MESHLET_QUADS);
}
