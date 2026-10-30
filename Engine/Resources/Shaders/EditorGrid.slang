#pragma once
#include "Includes/SceneGlobals.slang"

// Mirror of FEditorGridPC in SceneRenderer_Editor.cpp, with the matrices and vectors first.
struct FEditorGridArgs
{
    float4x4   ViewProjection;
    float4x4   InverseViewProjection;
    float4     MinorColor;
    float4     MajorColor;
    float4     AxisXColor;
    float4     AxisZColor;
    float      Spacing;
    float      FadeDistance;
    float      LineWidth;
    float      AxisWidth;
    FTexture2D Depth;
    uint       _Pad0;
    uint       _Pad1;
    uint       _Pad2;
};
FEditorGridArgs* Pass() { return GetPassArgs<FEditorGridArgs>(); }

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

static const float kMinCellPixels = 6.0;

float3 Unproject(float2 UV, float Depth)
{
    const float4 World = mul(Pass().InverseViewProjection, float4(UV * 2.0 - 1.0, Depth, 1.0));
    return World.xyz / World.w;
}

// Ben Golus' pristine grid: a line keeps its requested width until it would be thinner than a pixel, then fades to its average coverage.
float PristineGrid(float2 UV, float2 UVDeriv, float2 LineWidth)
{
    const bool2  bInvert     = LineWidth > 0.5;
    const float2 TargetWidth = select(bInvert, 1.0 - LineWidth, LineWidth);
    const float2 DrawWidth   = clamp(TargetWidth, UVDeriv, 0.5);
    const float2 LineAA      = max(UVDeriv, 0.000001) * 1.5;

    float2 GridUV = abs(frac(UV) * 2.0 - 1.0);
    GridUV = select(bInvert, GridUV, 1.0 - GridUV);

    float2 Grid2 = smoothstep(DrawWidth + LineAA, DrawWidth - LineAA, GridUV);
    Grid2 *= saturate(TargetWidth / DrawWidth);
    Grid2  = lerp(Grid2, TargetWidth, saturate(UVDeriv * 2.0 - 1.0));
    Grid2  = select(bInvert, 1.0 - Grid2, Grid2);

    return lerp(Grid2.x, 1.0, Grid2.y);
}

float GridLevel(float2 Coord, float2 WorldDeriv, float Cell, float WidthPixels)
{
    const float2 UVDeriv = WorldDeriv / Cell;
    return PristineGrid(Coord / Cell, UVDeriv, min(UVDeriv * WidthPixels, 1.0));
}

float AxisLine(float DistancePixels, float WidthPixels)
{
    return saturate(WidthPixels * 0.5 + 0.5 - DistancePixels);
}

float4 Over(float4 Below, float3 Color, float Alpha)
{
    return float4(Color * Alpha + Below.rgb * (1.0 - Alpha), Alpha + Below.a * (1.0 - Alpha));
}

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    const float3 Near = Unproject(Input.UV, 1.0);
    const float3 Dir  = Unproject(Input.UV, 0.5) - Near;

    // A ray that never reaches the plane still lands somewhere, so the derivatives below stay defined across the horizon.
    const float  Denominator = abs(Dir.y) > 1e-7 ? Dir.y : 1e-7;
    const float  T           = -Near.y / Denominator;
    const bool   bHits       = T > 0.0;
    const float3 Hit         = Near + Dir * max(T, 0.0);

    const float2 Coord      = Hit.xz;
    const float2 WorldDeriv = max(float2(length(float2(ddx(Coord.x), ddy(Coord.x))),
                                         length(float2(ddx(Coord.y), ddy(Coord.y)))), 1e-6);

    const float Spacing = max(Pass().Spacing, 1e-4);
    const float Lod     = max(log10(max(WorldDeriv.x, WorldDeriv.y) * kMinCellPixels / Spacing), -1.0);
    const float Fade    = frac(Lod);
    const float Fine    = Spacing * pow(10.0, floor(Lod) + 1.0);

    const float4 Minor = Pass().MinorColor;
    const float4 Major = Pass().MajorColor;
    const float  Width = Pass().LineWidth;

    float4 Result = float4(0.0, 0.0, 0.0, 0.0);
    Result = Over(Result, Major.rgb, Major.a * Fade * GridLevel(Coord, WorldDeriv, Fine * 100.0, Width));
    Result = Over(Result, lerp(Major.rgb, Minor.rgb, Fade), lerp(Major.a, Minor.a, Fade) * GridLevel(Coord, WorldDeriv, Fine * 10.0, Width));
    Result = Over(Result, Minor.rgb, Minor.a * (1.0 - Fade) * GridLevel(Coord, WorldDeriv, Fine, Width));

    const float2 AxisDistance = abs(Coord) / WorldDeriv;
    Result = Over(Result, Pass().AxisZColor.rgb, Pass().AxisZColor.a * AxisLine(AxisDistance.x, Pass().AxisWidth));
    Result = Over(Result, Pass().AxisXColor.rgb, Pass().AxisXColor.a * AxisLine(AxisDistance.y, Pass().AxisWidth));

    const float3 Camera       = GetCameraPosition();
    const float  Reach        = max(Pass().FadeDistance, abs(Camera.y) * 10.0);
    const float  DistanceFade = 1.0 - smoothstep(Reach * 0.35, Reach, length(Hit.xz - Camera.xz));
    const float  HorizonFade  = saturate(abs(normalize(Dir).y) * 25.0);

    // Reverse-Z, so the scene hides the grid where it is nearer; the margin keeps a floor at the plane from flickering over it.
    const float4 HitClip    = mul(Pass().ViewProjection, float4(Hit, 1.0));
    const float  HitDepth   = HitClip.z / HitClip.w;
    const float  SceneDepth = Pass().Depth.SampleLevel(Samplers::PointClamp, Input.UV, 0.0).r;
    const float  Visible    = SceneDepth > HitDepth * 1.002 + 1e-7 ? 0.0 : 1.0;

    Result *= (bHits ? 1.0 : 0.0) * Visible * DistanceFade * HorizonFade;
    if (Result.a <= 0.0)
    {
        discard;
    }

    return Result;
}
