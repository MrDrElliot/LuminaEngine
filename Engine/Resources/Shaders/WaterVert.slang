#include "Includes/Water.slang"

// Procedural water grid built from SV_VertexID with no vertex buffer, then displaced by the wave sum.

struct VSOutput
{
    float4 Position : SV_Position;
    float3 WorldPos : WATER_WORLDPOS;
    float2 BaseXZ   : WATER_BASEXZ;   // undisplaced world XZ, where the pixel shader evaluates the waves
    nointerpolation uint WaterIndex : WATER_INDEX;
};

// World spacing wanted between vertices directly under the camera.
static const float kNearCellSize = 0.6;

// Dense at the center and cubic toward the edge, so detail is spent near the camera and the far sea stays cheap.
float WarpGrid(float U, float Linear)
{
    float A = abs(U);
    return sign(U) * (Linear * A + (1.0 - Linear) * A * A * A);
}

float WarpGridSlope(float U, float Linear)
{
    return Linear + 3.0 * (1.0 - Linear) * U * U;
}

[shader("vertex")]
VSOutput VertexMain(uint VertexID : SV_VulkanVertexID, uint WaterIndex : SV_VulkanInstanceID)
{
    FGPUWater W = Pass().Waters.Get(WaterIndex);

    uint Res   = max(W.GridResolution, 2u);
    uint Cells = Res - 1u;

    uint Cell   = VertexID / 6u;
    uint Corner = VertexID % 6u;
    uint cx = Cell % Cells;
    uint cy = Cell / Cells;

    const uint2 kCorners[6] = { uint2(0, 0), uint2(1, 0), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(0, 1) };
    uint2 Off = kCorners[Corner];
    uint  gx  = min(cx + Off.x, Cells);
    uint  gy  = min(cy + Off.y, Cells);

    float  CellU    = 2.0 / float(Cells);
    float2 U        = float2(float(gx), float(gy)) * CellU - 1.0;
    float  WorldX   = length(mul(W.WaterToWorld, float4(1.0, 0.0, 0.0, 0.0)).xyz);
    float  WorldZ   = length(mul(W.WaterToWorld, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    float  Extent   = max(max(WorldX, WorldZ), 1.0);
    float  Linear   = clamp(kNearCellSize / (CellU * Extent), 0.02, 1.0);

    // Centered on the camera, snapped to the finest cell so vertices under the camera never swim as it moves.
    float2 CameraLocal = clamp(mul(W.WorldToWater, float4(GetCameraPosition(), 1.0)).xz, -0.5, 0.5);
    float  Snap        = Linear * CellU;
    CameraLocal = round(CameraLocal / Snap) * Snap;

    float2 Unclamped = CameraLocal + float2(WarpGrid(U.x, Linear), WarpGrid(U.y, Linear));
    float2 Local     = clamp(Unclamped, -0.5, 0.5);
    float  CellSize  = max(WarpGridSlope(U.x, Linear), WarpGridSlope(U.y, Linear)) * CellU * Extent;

    // Vertices folded onto the body's edge stay flat, so the collapsed strip there has no height to show.
    if (any(Local != Unclamped))
    {
        CellSize = 1e6;
    }

    // Only the outermost ring stretches, so the grid keeps its density near the camera while the sea runs to the horizon.
    if (W.HorizonScale > 1.0 && (gx == 0u || gy == 0u || gx == Cells || gy == Cells))
    {
        Local   *= W.HorizonScale;
        CellSize = 1e6;
    }

    float3 WorldBase = mul(W.WaterToWorld, float4(Local.x, 0.0, Local.y, 1.0)).xyz;
    float3 WorldPos  = WorldBase + WaterDisplacement(WorldBase.xz, W, GetTime(), CellSize);

    VSOutput O;
    O.Position   = WorldToClip(WorldPos);
    O.WorldPos   = WorldPos;
    O.BaseXZ     = WorldBase.xz;
    O.WaterIndex = WaterIndex;
    return O;
}
