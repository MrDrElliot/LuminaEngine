#pragma once
#include "GlobalRHI.slang"

// Shared layout for the fully-bindless RmlUi pass: per-draw data lives in a device-address buffer
// indexed per-vertex, so the whole UI draws in one DrawIndexed. Scissor is done in the FS (ClipRect).

// Draw kinds. TEXTURED is the ordinary path; the gradients are RmlUi's shader decorators, where UV
// carries the element-local pixel position rather than a texture coordinate.
#define UI_SHADER_TEXTURED 0u
#define UI_SHADER_LINEAR   1u
#define UI_SHADER_RADIAL   2u
#define UI_SHADER_CONIC    3u

struct FUiDraw
{
    float4x4 MVP;          // @0
    float4   ClipRect;     // @64  minX, minY, maxX, maxY in framebuffer pixels
    uint     TextureID;    // @80  global-heap sampled index (gTextures2D)
    uint     SamplerIndex; // @84  stock sampler heap slot
    uint     ShaderType;   // @88  UI_SHADER_*
    uint     StopOffset;   // @92  first colour stop in Stops[]
    float4   ShaderParams; // @96  linear: p0.xy p1.xy | radial: centre.xy radius.xy | conic: centre.xy dir.xy
    uint     StopCount;    // @112
    uint     bRepeating;   // @116
    float    ShaderScale;  // @120 linear only: 1 / dot(p1-p0, p1-p0)
    uint     ClipMaskRange;// @124 count in the low 8 bits, first ClipMasks[] index above them
};

#define UI_CLIP_COUNT(Range)  ((Range) & 0xFFu)
#define UI_CLIP_OFFSET(Range) ((Range) >> 8u)

// Analytic stand-in for RmlUi's clip mask. The mask is always an axis-aligned rounded rect in
// document space, so an SDF costs one varying instead of a stencil attachment. A draw carries a
// range of them because RmlUi intersects nested clips.
struct FUiClipMask
{
    float4 Rect;   // centre.xy, half-extent.zw in document space
    float4 Radii;  // top-left, top-right, bottom-right, bottom-left
    float4 Params; // .x nonzero keeps the area outside the shape instead of inside
};

// Position rides a float4 so both members stay 16-byte aligned under PhysicalStorageBuffer rules.
struct FUiColorStop
{
    float4 Color;      // premultiplied
    float4 Position;   // .x = normalised position along the gradient line
};

// Mirrors FUiVertex in RmlUiRenderer.h (stride 24); pulled by device address (no input layout).
// Order matters: the two float2s are kept adjacent (offsets 0 and 8) so neither vector straddles a
// 16-byte boundary under PhysicalStorageBuffer layout rules. Colour between them would put UV at
// offset 12, which spirv-val rejects (VUID-VkShaderModuleCreateInfo-pCode-08737).
struct FUiVertex
{
    float2 Position;   // @0
    float2 UV;         // @8
    uint   Colour;     // @16  premultiplied RGBA8
    uint   DrawIndex;  // @20  index into Draws[]
};

// Args block reached through the single 8-byte push constant (gRHI.Args).
struct FRmlUiArgs
{
    FUiDraw*      Draws;      // device address of the per-draw array (transient)
    FUiVertex*    Vertices;   // device address of the resident batch vertex buffer (vertex pulling)
    FUiColorStop* Stops;      // device address of the gradient colour stops (transient, never null)
    FUiClipMask*  ClipMasks;  // device address of the rounded-rect clip masks (transient, never null)
};

FRmlUiArgs* Pass() { return GetArgs<FRmlUiArgs>(); }
