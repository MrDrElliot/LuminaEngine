#pragma once
#include "SceneGlobals.slang"

// Shared data for the world-space text pass (TextVert / TextPixel). Each glyph is an instanced quad whose
// world plane is (Origin, Right, Up); the MSDF atlas is sampled and resolved with the median-of-three rule.
// The per-glyph array arrives by device address and the atlas by bindless index, both in the push constant.

// Must match FGPUGlyph in SceneRenderTypes.h (scalar layout, 96B).
struct FGPUGlyph
{
    float3 Origin;   float _p0;   // world anchor (entity origin)
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
    GPUSpan<FGPUGlyph> Glyphs;   // the per-frame glyph array, indexed by instance
    uint  AtlasIndex;        // bindless ResourceID of the font MSDF atlas
    uint  AtlasWidth;
    uint  AtlasHeight;
    float DistanceRange;     // px distance range baked into the MSDF (drives screen-space AA)
    uint  ScreenWidth;       // viewport pixel size (debug screen-space text only; 0 for world text)
    uint  ScreenHeight;
};
FTextPushConstants* Pass() { return GetPassArgs<FTextPushConstants>(); }

// Quad corners, indexed by SV_VertexID. x: 0=left/1=right, y: 0=bottom/1=top (in the text plane).
static const float2 kTextQuadCorners[6] =
{
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0)
};

float TextMedian(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}
