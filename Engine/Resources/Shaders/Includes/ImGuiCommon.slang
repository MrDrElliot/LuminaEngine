#pragma once
#include "GlobalRHI.slang"
#include "Tonemap.slang"

// Args re-point per draw as TextureID changes.

// HDR texels are linear radiance and get the scene's exposure, tone map and encode, or they read far too dark.
#define IMGUI_DISPLAY_DIRECT 0u
#define IMGUI_DISPLAY_HDR    1u
#define IMGUI_DISPLAY_LINEAR 2u   // sRGB encode, for a texture whose sampler hands back linear values

// The exact inverse of the hardware sRGB decode, so an sRGB texture's texel reaches the display as authored.
float3 ImGuiEncodeSRGB(float3 Linear)
{
    const float3 C    = saturate(Linear);
    const float3 Low  = C * 12.92;
    const float3 High = 1.055 * pow(C, float3(1.0 / 2.4)) - 0.055;
    return lerp(High, Low, float3(C <= float3(0.0031308)));
}

struct FImGuiArgs
{
    float2   Scale;        // 2 / DisplaySize
    float2   Translate;    // -(1 + DisplayPos * Scale)
    uint     TextureID;    // heap ResourceID, read as FTexture2DArray when bIsArray
    FSampler Sampler;
    uint     DisplayMode;  // IMGUI_DISPLAY_*
    float    Exposure;     // linear multiplier, HDR mode only
    // The descriptor type must match the image, so a Tex2DArray read as FTexture2D resolves to the null slot.
    uint     ArraySlice;   // layer to sample, bIsArray only
    uint     bIsArray;     // 0 = Texture2D, 1 = Texture2DArray
    uint64_t VertexAddr;   // device address of the transient ImDrawVert array
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
    float4 Position : SV_Position;
    float4 Color    : COLOR;
    float2 UV       : TEXCOORD0;
};
