#pragma once
#include "SceneGlobals.slang"
#include "Noise.slang"

// Mirrors GGroundFogMaxLayers in EnvironmentRenderTypes.h.
#define GROUND_FOG_MAX_LAYERS 8u

// Mirrors FGPUGroundFogLayer in EnvironmentRenderTypes.h.
struct FGroundFogLayer
{
    float4 WorldToLayerX;
    float4 WorldToLayerZ;
    // x = ground height, y = thickness, z = top softness as a fraction of the thickness, w = extinction
    float4 Shape;
    // rgb = albedo, w = scattering intensity
    float4 Albedo;
    // xy = edge fade in layer units along x and z, z = fade-out distance from the camera, w = anisotropy
    float4 Edge;
    // x = wisp strength, y = inverse wisp scale, zw = distance the drift has carried the wisps along x and z
    float4 Wisps;
};

// Mirrors FGPUGroundFogSim in EnvironmentRenderTypes.h.
struct FGroundFogSim
{
    float2 Origin;
    float  TexelWorld;
    uint   Resolution;
    int2   Shift;
    float  Dt;
    uint   bIngest;
    float4 ObstacleU;
    float4 ObstacleV;
    float2 DepthToY;
    float  BandBottom;
    float  BandTop;
    float2 Wind;
    float  VelocityKeep;
    float  SwirlKeep;
    float  RefillRate;
    float  Displacement;
    float  Vorticity;
    float  MaxSpeed;
};

// The layer may pile up this far above its rest thickness where the flow converges.
static const float kGroundFogMaxRise = 1.35;
// Fog fills dips in the ground down to this fraction of its thickness below the layer's height.
static const float kGroundFogBelow   = 0.5;
static const float3 kGroundFogHashOffset = float3(0.37, 0.11, 0.73);

// The simulated column at a point, Fill as a fraction of the rest thickness and WispOffset in meters the flow carried the wisps.
struct FGroundFogColumn
{
    float  Fill;
    float2 WispOffset;
    float  Stir;
};

FGroundFogColumn GroundFog_RestColumn()
{
    FGroundFogColumn Column;
    Column.Fill       = 1.0;
    Column.WispOffset = float2(0.0, 0.0);
    Column.Stir       = 0.0;
    return Column;
}

// Relaxes to the rest column toward the window's edge, so leaving the simulated area shows no seam.
FGroundFogColumn GroundFog_SampleColumn(FTexture2D Flow, FTexture2D Fill, float2 Origin, float InvWidth, float2 WorldXZ)
{
    FGroundFogColumn Column = GroundFog_RestColumn();
    if (!Flow.IsValid())
    {
        return Column;
    }

    const float2 UV   = (WorldXZ - Origin) * InvWidth;
    const float2 Edge = min(UV, 1.0 - UV);
    const float  Keep = saturate(min(Edge.x, Edge.y) * 12.0);
    if (Keep <= 0.0)
    {
        return Column;
    }

    const float4 FlowState = Flow.SampleLevel(Samplers::LinearClamp, UV, 0.0);
    const float  FillState = Fill.SampleLevel(Samplers::LinearClamp, UV, 0.0).x;
    Column.Fill       = lerp(1.0, FillState, Keep);
    Column.WispOffset = FlowState.zw * Keep;
    Column.Stir       = length(FlowState.xy) * Keep;
    return Column;
}

float2 GroundFog_LayerUV(FGroundFogLayer Layer, float3 WorldPos)
{
    const float4 P = float4(WorldPos, 1.0);
    return float2(dot(Layer.WorldToLayerX, P), dot(Layer.WorldToLayerZ, P));
}

// Coverage of the layer's area at a point, ramping to zero over the edge fade.
float GroundFog_AreaCoverage(FGroundFogLayer Layer, float3 WorldPos)
{
    const float2 Local = abs(GroundFog_LayerUV(Layer, WorldPos));
    const float2 Ramp  = saturate((1.0 - Local) / max(Layer.Edge.xy, 1.0e-4));
    return Ramp.x * Ramp.y;
}

// Extinction at a point, where the column's fill sets how high the layer stands and the wisps break it into banks.
float GroundFog_Extinction(FGroundFogLayer Layer, FGroundFogColumn Column, float3 WorldPos, float Time)
{
    const float Thickness = Layer.Shape.y;
    const float Height    = WorldPos.y - Layer.Shape.x;
    if (Height < -Thickness * kGroundFogBelow)
    {
        return 0.0;
    }

    const float Coverage = GroundFog_AreaCoverage(Layer, WorldPos);
    if (Coverage <= 0.0)
    {
        return 0.0;
    }

    float Wisp = 0.5;
    if (Layer.Wisps.x > 0.0)
    {
        // Slow boil through y keeps the banks changing shape as they drift, and the stirred flow drags them along.
        const float2 Drifted = WorldPos.xz - Layer.Wisps.zw - Column.WispOffset;
        const float3 P = float3(Drifted.x, Height * 2.5 - Time * 0.08, Drifted.y) * Layer.Wisps.y;
        Wisp = FBM3DNormalized(P, 3, 0.5, 2.03, kGroundFogHashOffset);
    }

    // Wisps lift and sink the top surface, so the layer rolls in banks rather than lying as a flat sheet.
    const float Lift = 1.0 + (Wisp - 0.5) * 1.6 * Layer.Wisps.x;
    const float Top  = Thickness * clamp(Column.Fill, 0.0, kGroundFogMaxRise) * max(Lift, 0.0);
    const float Soft = max(Top * Layer.Shape.z, 1.0e-3);
    const float Fall = saturate((Top - Height) / Soft);

    const float Thinning = lerp(1.0, saturate(Wisp * 1.6), Layer.Wisps.x * 0.5);
    return Layer.Shape.w * Coverage * Fall * Fall * Thinning;
}

// Entry and exit distances along a ray through the slab the layer can occupy, clipped to its area and empty when the ray misses.
float2 GroundFog_RaySegment(FGroundFogLayer Layer, float3 Origin, float3 Dir, float TMax)
{
    const float Bottom = Layer.Shape.x - Layer.Shape.y * kGroundFogBelow;
    const float Top    = Layer.Shape.x + Layer.Shape.y * kGroundFogMaxRise * 1.4;

    float T0 = 0.0;
    float T1 = TMax;
    if (abs(Dir.y) > 1.0e-6)
    {
        const float A = (Bottom - Origin.y) / Dir.y;
        const float B = (Top - Origin.y) / Dir.y;
        T0 = max(T0, min(A, B));
        T1 = min(T1, max(A, B));
    }
    else if (Origin.y < Bottom || Origin.y > Top)
    {
        return float2(1.0, 0.0);
    }

    // The area's two axes as slabs in layer units, which an affine map leaves linear along the ray.
    const float2 Start = GroundFog_LayerUV(Layer, Origin);
    const float2 Step  = float2(dot(Layer.WorldToLayerX.xyz, Dir), dot(Layer.WorldToLayerZ.xyz, Dir));
    [unroll]
    for (int Axis = 0; Axis < 2; ++Axis)
    {
        if (abs(Step[Axis]) > 1.0e-6)
        {
            const float A = (-1.0 - Start[Axis]) / Step[Axis];
            const float B = (1.0 - Start[Axis]) / Step[Axis];
            T0 = max(T0, min(A, B));
            T1 = min(T1, max(A, B));
        }
        else if (abs(Start[Axis]) > 1.0)
        {
            return float2(1.0, 0.0);
        }
    }
    return float2(T0, T1);
}
