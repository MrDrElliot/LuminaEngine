#pragma once

// The lattice is turned off the axes, so flat panels built along them cut irregular flakes instead of square pixels.
static const float3x3 kFlakeLattice = float3x3(
    0.865757, -0.280337, 0.414580,
    0.414580, 0.865757, -0.280337,
    -0.280337, 0.414580, 0.865757);

// A flake needs a couple of pixels to read as a glint, so below that it settles back to the smooth base instead of shimmering.
static const float kFlakeMinPixels = 1.5;

struct FMetallicFlakes
{
    float3 Normal;       // tangent space, encoded like a normal map sample for the Bottom Normal pin
    float  Glint;        // random per flake, for varying the flakes' color or roughness
    float  Visibility;   // 1 where flakes span a few pixels, 0 where they are too small to resolve
};

// Integer pcg3d, since a float hash of a cell index loses its spread at large coordinates.
uint3 FlakeHash(int3 Cell)
{
    uint3 V = uint3(Cell) * 1664525u + 1013904223u;
    V.x += V.y * V.z;
    V.y += V.z * V.x;
    V.z += V.x * V.y;
    V ^= V >> 16u;
    V.x += V.y * V.z;
    V.y += V.z * V.x;
    V.z += V.x * V.y;
    return V;
}

// Each cell of Size in Position's units is one flake, tilted at random by up to Tilt and faded by the pixel footprint.
FMetallicFlakes EvaluateMetallicFlakes(float3 Position, float3 PositionDdx, float3 PositionDdy, float Size, float Tilt)
{
    const float CellSize  = max(Size, 1e-6);
    const float Footprint = max(max(length(PositionDdx), length(PositionDdy)), 1e-9);
    const float3 H        = float3(FlakeHash(int3(floor(mul(kFlakeLattice, Position) / CellSize))) & 0xFFFFu) / 65535.0;

    FMetallicFlakes F;
    F.Visibility = saturate((CellSize / Footprint - kFlakeMinPixels) / kFlakeMinPixels);
    const float2 Offset = (H.xy * 2.0 - 1.0) * (Tilt * F.Visibility);
    F.Normal     = normalize(float3(Offset, 1.0)) * 0.5 + 0.5;
    F.Glint      = H.z;
    return F;
}
