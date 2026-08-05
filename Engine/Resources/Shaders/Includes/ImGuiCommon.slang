#pragma once
#include "GlobalRHI.slang"
#include "Tonemap.slang"

// Bindless ImGui pass: vertices pulled by device address, textures sampled from the
// global heap by ResourceID. Args re-point per draw (TextureID changes).

// How the sampled texel is turned into a display value.
//   DIRECT: pass through. Correct for UI atlases and cooked color textures, which already hold
//           display-encoded bytes.
//   HDR:    the texel is LINEAR radiance (RGBA16F environment maps, scene captures). Exposure ->
//           tone map -> display encode, matching what ColorGrading.slang does to the scene. Without
//           it the linear values reach an _UNORM swapchain unencoded and read far too dark.
#define IMGUI_DISPLAY_DIRECT 0u
#define IMGUI_DISPLAY_HDR    1u

struct FImGuiArgs
{
    float2   Scale;        // 2 / DisplaySize
    float2   Translate;    // -1 - DisplayPos * Scale
    uint     TextureID;    // heap ResourceID (gTextures2D[], or gTextures2DArray[] when bIsArray)
    uint     SamplerIndex; // SAMPLER_* heap slot
    uint     DisplayMode;  // IMGUI_DISPLAY_*
    float    Exposure;     // linear multiplier, HDR mode only
    // Array previews. The heap arrays are aliased views of one descriptor array, so TextureID is the
    // same ResourceID either way -- but the DESCRIPTOR TYPE has to match how the image was created, so
    // a Tex2DArray must be read through gTextures2DArray[] or it resolves to the null slot.
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
