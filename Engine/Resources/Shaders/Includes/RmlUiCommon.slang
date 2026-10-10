#pragma once

import RHI;
#include "GlobalRHI.slang"

// One device-address draw array indexed per vertex lets the whole UI draw in one DrawIndexed, with scissor done in the FS.

// For the gradient kinds UV carries the element-local pixel position rather than a texture coordinate.
#define kUIShaderTextured 0u
#define UI_SHADER_LINEAR   1u
#define UI_SHADER_RADIAL   2u
#define UI_SHADER_CONIC    3u
#define kUIShaderTexturedStraight 4u // asset texture with straight alpha; premultiplied at sample time
#define kUIShaderTexturedStraightSRGB 5u // the same from an sRGB texture, which the sampler hands back linear

// The exact inverse of the hardware sRGB decode, so an asset texel reaches the display-encoded target as authored.
float3 EncodeSRGB(float3 Linear)
{
    const float3 C    = saturate(Linear);
    const float3 Low  = C * 12.92;
    const float3 High = 1.055 * pow(C, float3(1.0 / 2.4)) - 0.055;
    return lerp(High, Low, float3(C <= float3(0.0031308)));
}

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

#define UI_CLIP_COUNT(Range)  ((Range) & 0xFFu)
#define UI_CLIP_OFFSET(Range) ((Range) >> 8u)

// An axis-aligned rounded rect in the clip element's own space, so an SDF replaces a stencil attachment.
struct FUiClipMask
{
    float4 Rect;         // center.xy, half-extent.zw in the mask's own space
    float4 Radii;        // top-left, top-right, bottom-right, bottom-left
    float4 Params;       // .x nonzero keeps the area outside the shape, .yz translate InvTransform
    float4 InvTransform; // row-major 2x2 taking a framebuffer pixel into the mask's own space
};

// RmlUi transforms each clip element separately, so the fragment moves into the mask rather than the reverse.
float2 UiClipLocal(FUiClipMask Mask, float2 FragCoord)
{
    return float2(dot(Mask.InvTransform.xy, FragCoord), dot(Mask.InvTransform.zw, FragCoord)) + Mask.Params.yz;
}

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
    uint   Colour;     // @16  premultiplied RGBA8
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
