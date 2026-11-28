#pragma once
#include "GlobalRHI.slang"
#include "Tonemap.slang"

// HDR texels are linear radiance and get the scene's exposure, tone map and encode, or they read far too dark.
#define IMGUI_DISPLAY_DIRECT 0u
#define IMGUI_DISPLAY_HDR    1u
#define IMGUI_DISPLAY_LINEAR 2u   // sRGB encode, for a texture whose sampler hands back linear values
#define IMGUI_DISPLAY_SCENE  3u   // a scene display image, drawn opaque so its alpha HDR gain reaches the composite

// One block for every ImGui command in the draw data, which indexes its own FImGuiDraw by first instance.
struct FImGuiArgs
{
    float2   Scale;        // 2 / DisplaySize
    float2   Translate;    // -(1 + DisplayPos * Scale)
    float2   ClipOffset;   // DisplayPos
    float2   ClipScale;    // FramebufferScale
    FSampler Sampler;
    uint64_t VertexAddr;   // device address of the transient ImDrawVert array
    uint64_t DrawsAddr;    // device address of the transient FImGuiDraw array
};

struct FImGuiDraw
{
    float4 ClipRect;       // framebuffer pixels, min corner then max corner
    uint   TextureID;      // heap ResourceID, read as FTexture2DArray when bIsArray
    uint   DisplayMode;    // IMGUI_DISPLAY_*
    float  Exposure;       // linear multiplier, HDR mode only
    // The descriptor type must match the image, so a Tex2DArray read as FTexture2D resolves to the null slot.
    uint   ArraySlice;     // layer to sample, bIsArray only
    uint   bIsArray;       // 0 = Texture2D, 1 = Texture2DArray
};

// Mirrors ImDrawVert (pos float2 @0, uv float2 @8, col RGBA8 @16), stride 20.
struct ImDrawVert
{
    float2 Position;
    float2 UV;
    uint   Color;
};

struct FImGuiInterpolants
{
    // The clip rect, as a scissor per command would split the batch.
    float  Clip[4]                  : SV_ClipDistance;
    float4 Position                 : SV_Position;
    float4 Color                    : COLOR;
    float2 UV                       : TEXCOORD0;
    nointerpolation uint DrawIndex  : DRAW_INDEX;
};

FImGuiDraw GetDraw(FImGuiArgs* Args, uint DrawIndex)
{
    return ((FImGuiDraw*)Args.DrawsAddr)[DrawIndex];
}
