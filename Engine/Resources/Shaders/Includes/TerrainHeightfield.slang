#pragma once
#include "Includes/GlobalRHI.slang"

// A terrain's heightmap as a world-space height field, which far sun shadows march and the world distance field merges.

// Covers a 4x4 open-world terrain tiling with shadow casting enabled on every tile.
static const uint kMaxTerrainHeightfields = 16u;

// Mirror of Lumina::FTerrainHeightfieldGPU.
struct FTerrainHeightfield
{
    float2     OriginXZ;
    float      Stride;
    float      OriginY;
    float      MaxHeight;
    float      Resolution;
    FTexture2D Heightmap;
    float      _Pad;

    float HeightAt(float2 XZ)
    {
        const float2 UV = ((XZ - OriginXZ) / Stride + 0.5) / Resolution;
        return OriginY + Heightmap.SampleLevel(Samplers::LinearClamp, UV, 0.0).r * MaxHeight;
    }

    bool Covers(float2 XZ)
    {
        const float2 Texel = (XZ - OriginXZ) / Stride;
        return all(Texel >= 0.0) && all(Texel <= Resolution - 1.0);
    }

    // Height above the surface scaled by its slope, which never overstates the true distance to a plane.
    float Distance(float3 P, float Step)
    {
        const float  Height   = HeightAt(P.xz);
        const float2 Gradient = float2(HeightAt(P.xz + float2(Step, 0.0)) - HeightAt(P.xz - float2(Step, 0.0)),
                                       HeightAt(P.xz + float2(0.0, Step)) - HeightAt(P.xz - float2(0.0, Step))) / (2.0 * Step);
        return (P.y - Height) / sqrt(1.0 + dot(Gradient, Gradient));
    }
};

// Coarse terrain LODs cut under the height field between vertices, and a receiver left there reads as buried in it.
float3 LiftOntoTerrains(float3 P, FTerrainHeightfield Terrains[kMaxTerrainHeightfields], uint NumTerrains)
{
    for (uint i = 0; i < min(NumTerrains, kMaxTerrainHeightfields); ++i)
    {
        if (Terrains[i].Covers(P.xz))
        {
            P.y = max(P.y, Terrains[i].HeightAt(P.xz));
        }
    }
    return P;
}
