#pragma once
#include "Includes/SceneGlobals.slang"

// Mirror of FEditorGridPC in SceneRenderer_Editor.cpp, with the vectors first.
struct FEditorGridArgs
{
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

// The camera's cell phase is one value for the whole screen, so far from the origin its rounding shifts the grid instead of warping it.
float GridLevel(float2 CameraXZ, float2 Offset, float2 WorldDeriv, float Cell, float WidthPixels)
{
    const float2 UVDeriv = WorldDeriv / Cell;
    return PristineGrid(frac(CameraXZ / Cell) + Offset / Cell, UVDeriv, min(UVDeriv * WidthPixels, 1.0));
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
    // Camera-relative, since a camera translation folded into the ray leaves too few float bits for the hit at height or far out.
    const float3 Camera  = GetCameraPosition();
    const float3 NearRel = mul((float3x3)GetInverseCameraView(), GetCameraViewRay(Input.UV + GetTemporalJitterUV()));
    const float3 Dir     = IsCameraOrthographic() ? GetCameraForward() : NearRel;

    // A ray that never reaches the plane still lands somewhere, so the derivatives below stay defined across the horizon.
    const float  Denominator = abs(Dir.y) > 1e-7 ? Dir.y : 1e-7;
    const float  T           = -(Camera.y + NearRel.y) / Denominator;
    const bool   bHits       = T > 0.0;
    const float3 Offset      = NearRel + Dir * max(T, 0.0);

    const float2 Coord      = Camera.xz + Offset.xz;
    const float2 WorldDeriv = max(float2(length(float2(ddx(Offset.x), ddy(Offset.x))),
                                         length(float2(ddx(Offset.z), ddy(Offset.z)))), 1e-6);

    const float Spacing = max(Pass().Spacing, 1e-4);
    const float Lod     = max(log10(max(WorldDeriv.x, WorldDeriv.y) * kMinCellPixels / Spacing), -1.0);
    const float Fade    = frac(Lod);
    const float Fine    = Spacing * pow(10.0, floor(Lod) + 1.0);

    const float4 Minor = Pass().MinorColor;
    const float4 Major = Pass().MajorColor;
    const float  Width = Pass().LineWidth;

    float4 Result = float4(0.0, 0.0, 0.0, 0.0);
    Result = Over(Result, Major.rgb, Major.a * Fade * GridLevel(Camera.xz, Offset.xz, WorldDeriv, Fine * 100.0, Width));
    Result = Over(Result, lerp(Major.rgb, Minor.rgb, Fade), lerp(Major.a, Minor.a, Fade) * GridLevel(Camera.xz, Offset.xz, WorldDeriv, Fine * 10.0, Width));
    Result = Over(Result, Minor.rgb, Minor.a * (1.0 - Fade) * GridLevel(Camera.xz, Offset.xz, WorldDeriv, Fine, Width));

    const float2 AxisDistance = abs(Coord) / WorldDeriv;
    Result = Over(Result, Pass().AxisZColor.rgb, Pass().AxisZColor.a * AxisLine(AxisDistance.x, Pass().AxisWidth));
    Result = Over(Result, Pass().AxisXColor.rgb, Pass().AxisXColor.a * AxisLine(AxisDistance.y, Pass().AxisWidth));

    const float  Reach        = max(Pass().FadeDistance, abs(Camera.y) * 10.0);
    const float  DistanceFade = 1.0 - smoothstep(Reach * 0.35, Reach, length(Offset.xz));
    const float  HorizonFade  = saturate(abs(normalize(Dir).y) * 25.0);

    // Reverse-Z, so the scene hides the grid where it is nearer; the margin keeps a floor at the plane from flickering over it.
    const float4 HitClip    = ViewToClip(mul((float3x3)GetCameraView(), Offset));
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
