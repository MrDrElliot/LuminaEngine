#include "Includes/Water.slang"

// Procedural water grid. Generates an (Res-1)^2-cell triangle list from SV_VertexID (no vertex buffer),
// places it in the body's local XZ plane, transforms to world, then displaces by the Gerstner sum.

struct VSOutput
{
    float4 Position : SV_Position;
    float3 WorldPos : WATER_WORLDPOS;
    float3 Normal   : WATER_NORMAL;
    float2 PlanarUV : WATER_UV;       // base grid uv in [0,1] for detail/foam tiling
    float  Jacobian : WATER_JACOBIAN; // drives whitecaps where the surface folds
    nointerpolation uint WaterIndex : WATER_INDEX;
};

[shader("vertex")]
VSOutput VertexMain(uint VertexID : SV_VulkanVertexID, uint WaterIndex : SV_VulkanInstanceID)
{
    FGPUWater W = Pass().Waters[WaterIndex];

    uint Res   = max(W.GridResolution, 2u);
    uint Cells = Res - 1u;

    uint Cell   = VertexID / 6u;
    uint Corner = VertexID % 6u;
    uint cx = Cell % Cells;
    uint cy = Cell / Cells;

    // Two triangles per cell: (0,0),(1,0),(1,1) and (0,0),(1,1),(0,1).
    const uint2 kCorners[6] = { uint2(0, 0), uint2(1, 0), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(0, 1) };
    uint2 Off = kCorners[Corner];
    uint  gx  = min(cx + Off.x, Cells);
    uint  gy  = min(cy + Off.y, Cells);

    float2 UV       = float2(float(gx), float(gy)) / float(Cells);   // [0,1]
    float3 LocalPos = float3(UV.x - 0.5, 0.0, UV.y - 0.5);
    float3 WorldBase = mul(W.WaterToWorld, float4(LocalPos, 1.0)).xyz;

    FGerstnerResult G = EvaluateGerstner(WorldBase.xz, W, GetTime());
    float3 WorldPos   = WorldBase + G.Displacement;

    VSOutput O;
    O.Position   = WorldToClip(WorldPos);
    O.WorldPos   = WorldPos;
    O.Normal     = G.Normal;
    O.PlanarUV   = UV;
    O.Jacobian   = G.Jacobian;
    O.WaterIndex = WaterIndex;
    return O;
}
