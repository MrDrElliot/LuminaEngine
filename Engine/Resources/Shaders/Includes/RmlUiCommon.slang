#pragma once

import RHI;
#include "GlobalRHI.slang"
#include "Tonemap.slang"
#include "UiClipMask.slang"

// One device-address draw array indexed per vertex lets the whole UI draw in one DrawIndexed, with scissor done in the FS.

// For the gradient kinds UV carries the element-local pixel position rather than a texture coordinate.
#define kUIShaderTextured 0u
#define UI_SHADER_LINEAR   1u
#define UI_SHADER_RADIAL   2u
#define UI_SHADER_CONIC    3u
#define kUIShaderTexturedStraight 4u // asset texture with straight alpha; premultiplied at sample time
#define kUIShaderTexturedStraightSRGB 5u // the same from an sRGB texture, which the sampler hands back linear

struct FUiDraw
{
    float4x4   MVP;          // @0
    float4     ClipRect;     // @64  minX, minY, maxX, maxY in framebuffer pixels
    FTexture2D Texture;    // @80
    FSampler   Sampler;    // @84
    uint       ShaderType;   // @88  UI_SHADER_*
    uint       StopOffset;   // @92  first color stop in Stops[]
    float4     ShaderParams; // @96  p0.xy p1.xy (linear), center.xy radius.xy (radial), center.xy dir.xy (conic)
    uint       StopCount;    // @112
    uint       bRepeating;   // @116
    float      ShaderScale;  // @120 1 / dot(p1-p0, p1-p0), linear only
    uint       ClipMaskRange;// @124 count in the low 8 bits, first ClipMasks[] index above them
};

// Position rides a float4 so both members stay 16-byte aligned under PhysicalStorageBuffer rules.
struct FUiColorStop
{
    float4 Color;      // premultiplied
    float4 Position;   // .x is the normalized position along the gradient line
};

// Mirrors FUiVertex in RmlUiRenderer.h (stride 24), and UV must stay at 8 since offset 12 fails spirv-val (VUID-08737).
struct FUiVertex
{
    float2 Position;   // @0
    float2 UV;         // @8
    uint   Color;      // @16  premultiplied RGBA8
    uint   DrawIndex;  // @20
};

struct FRmlUiArgs
{
    GPUSpan<FUiDraw>      Draws;      // transient
    GPUSpan<FUiVertex>    Vertices;   // resident batch, pulled by the vertex shader
    GPUSpan<FUiColorStop> Stops;      // transient, empty when unused
    GPUSpan<FUiClipMask>  ClipMasks;  // transient, empty when unused
};

FRmlUiArgs* Pass() { return GetArgs<FRmlUiArgs>(); }
