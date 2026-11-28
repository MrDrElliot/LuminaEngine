#pragma once
#include "Includes/TerrainCommon.slang"

// Terrain records as world-space height fields, which far sun shadows march, the world distance field merges and DFAO lifts receivers onto.

// Every casting terrain, not only those in view, since a ridge behind the camera still shades and occludes what is in front.
bool TerrainIsHeightfield(FTerrainRecord* Record)
{
    return (Record.Params.Flags & TERRAIN_FLAG_CAST_SHADOW) != 0u && !Record.PageTable.IsEmpty();
}

// World size of a pixel this far from the cull camera, the detail the page streamer keeps resident there.
float TerrainViewFootprint(float Distance)
{
    const float PixelsPerUnit = abs(GetCullCameraProjection()[1][1]) * 0.5 * float(Scene().ScreenSize.y);
    return Distance / max(PixelsPerUnit, 1e-3);
}

float TerrainWorldHeight(FTerrainRecord* Record, float2 XZ, uint Mip, inout FTerrainPage Cache)
{
    return Record.Params.WorldOriginY.x + TerrainHeightAt(Record, TerrainSamplePos(Record, XZ), Mip, Cache);
}

float TerrainWorldHeight(FTerrainRecord* Record, float2 XZ, float Footprint)
{
    FTerrainPage Cache = (FTerrainPage)0;
    return TerrainWorldHeight(Record, XZ, (uint)TerrainFootprintMip(Record, Footprint), Cache);
}

// Height above the surface scaled by its slope, which never overstates the true distance to a plane.
float TerrainSurfaceDistance(FTerrainRecord* Record, float3 P, float Step)
{
    const uint   Mip = (uint)TerrainFootprintMip(Record, Step);
    FTerrainPage Cache = (FTerrainPage)0;
    const float  Height   = TerrainWorldHeight(Record, P.xz, Mip, Cache);
    const float2 Gradient = float2(TerrainWorldHeight(Record, P.xz + float2(Step, 0.0), Mip, Cache) - TerrainWorldHeight(Record, P.xz - float2(Step, 0.0), Mip, Cache),
                                   TerrainWorldHeight(Record, P.xz + float2(0.0, Step), Mip, Cache) - TerrainWorldHeight(Record, P.xz - float2(0.0, Step), Mip, Cache)) / (2.0 * Step);
    return (P.y - Height) / sqrt(1.0 + dot(Gradient, Gradient));
}

// Coarse terrain LODs cut under the height field between vertices, and a receiver left there reads as buried in it.
float3 LiftOntoTerrains(float3 P)
{
    const GPUSpan<FTerrainRecord> Terrains = GetSceneRoot().Terrains;
    const float Footprint = TerrainViewFootprint(length(P - GetCameraPosition()));
    for (uint i = 0; i < Terrains.Count; ++i)
    {
        FTerrainRecord* Terrain = Terrains.At(i);
        if (TerrainIsHeightfield(Terrain) && TerrainCovers(Terrain, P.xz))
        {
            P.y = max(P.y, TerrainWorldHeight(Terrain, P.xz, Footprint));
        }
    }
    return P;
}
