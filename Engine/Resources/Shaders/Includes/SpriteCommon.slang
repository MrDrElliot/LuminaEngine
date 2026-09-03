#pragma once
#include "SceneGlobals.slang"

// Sheet slicing, region and flips are all resolved into UVRect on the CPU.

// Must match FGPUSprite in SceneRenderTypes.h (scalar layout, 96B).
struct FGPUSprite
{
    float3 Origin;   float _p0;   // world anchor, the entity origin
    float3 Right;    float _p1;   // world right axis, unit length
    float3 Up;       float _p2;   // world up axis, unit length
    float4 UVRect;                // u0, v0 top-left, then u1, v1 bottom-right
    float2 PlaneMin;              // quad min in world units, relative to Origin
    float2 PlaneMax;              // quad max in world units, relative to Origin
    uint   Color;                 // packed RGBA
    uint   EntityID;              // for the picker pass
    uint   Flags;
    float  AlphaCutThreshold;
};

static const uint SPRITE_FLAG_ALPHA_CUT = 1u << 0;

struct FSpritePushConstants
{
    FGPUSprite* Instances;   // per-frame sprite array, indexed by instance
    uint  TextureIndex;      // bindless ResourceID of the sprite texture
    uint  _p0;
};
FSpritePushConstants* Pass() { return GetPassArgs<FSpritePushConstants>(); }

// Indexed by SV_VertexID. x runs left to right, y runs bottom to top, in the sprite plane.
static const float2 kSpriteQuadCorners[6] =
{
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0)
};
