#pragma once
#include "SceneGlobals.slang"

// Must match FGPUGlyph in SceneRenderTypes.h (scalar layout, 96B).
struct FGPUGlyph
{
    float3 Origin;   float Intensity;   // World anchor and HDR color scale
    float3 Right;    float _p1;   // world right axis * worldEmSize
    float3 Up;       float _p2;   // world up axis * worldEmSize
    float4 UVRect;                // u0, v0 (top-left), u1, v1 (bottom-right)
    float2 PlaneMin;              // quad min in the text plane (em units)
    float2 PlaneMax;              // quad max in the text plane (em units)
    uint   Color;                 // packed RGBA
    uint   EntityID;              // for the picker pass
    uint   _p4;
    uint   _p5;
};

struct FTextPushConstants
{
    GPUSpan<FGPUGlyph> Glyphs;
    FTexture2D         Atlas;
    uint               AtlasWidth;
    uint               AtlasHeight;
    float              DistanceRange;     // Pixel distance range baked into the MSDF, drives screen-space AA
    uint               ScreenWidth;       // Debug screen-space text only, 0 for world text
    uint               ScreenHeight;
};
FTextPushConstants* Pass() { return GetPassArgs<FTextPushConstants>(); }

// x is 0 left and 1 right, y is 0 bottom and 1 top, in the text plane.
static const float2 kTextQuadCorners[6] =
{
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0)
};

float TextMedian(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}
