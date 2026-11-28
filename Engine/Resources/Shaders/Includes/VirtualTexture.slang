#pragma once
#include "SceneGlobals.slang"

// Runtime virtual textures lay a wrapped image over the world's XZ plane, so a world texel lives at itself modulo the resolution.

// What a capture writes for each EVirtualTextureLayout, matching the C++ enum's order.
#define VIRTUAL_TEXTURE_LAYOUT_BASECOLOR_ROUGHNESS 0u
#define VIRTUAL_TEXTURE_LAYOUT_NORMAL_HEIGHT       1u
#define VIRTUAL_TEXTURE_LAYOUT_BASECOLOR_HEIGHT    2u

// The window fades to its clear value over this many texels, so nothing ends in a seam as it scrolls.
#define VIRTUAL_TEXTURE_EDGE_FADE_TEXELS 8.0

struct FVirtualTextureSample
{
    float4 Value;
    // One inside the window, fading to zero at its edge and outside it.
    float  Valid;
};

uint2 VirtualTextureImageTexel(int2 WorldTexel, uint Resolution)
{
    const int R = int(Resolution);
    return uint2(((WorldTexel % R) + R) % R);
}

float4 VirtualTextureCaptureValue(uint Layout, float3 BaseColor, float Roughness, float3 NormalWS, float Height01)
{
    switch (Layout)
    {
    case VIRTUAL_TEXTURE_LAYOUT_NORMAL_HEIGHT:    return float4(NormalWS * 0.5 + 0.5, Height01);
    case VIRTUAL_TEXTURE_LAYOUT_BASECOLOR_HEIGHT: return float4(BaseColor, Height01);
    default:                                      return float4(BaseColor, Roughness);
    }
}

// Zero with no validity for a texture nothing has written, since its clear value is unknown here.
FVirtualTextureSample SampleVirtualTexture(uint TextureIndex, float2 WorldXZ)
{
    FVirtualTextureSample Result;
    Result.Value = float4(0.0, 0.0, 0.0, 0.0);
    Result.Valid = 0.0;

    const GPUSpan<FVirtualTextureData> Textures = Scene().VirtualTextures;
    for (uint i = 0u; i < Textures.Count; ++i)
    {
        const FVirtualTextureData V = Textures.Get(i);
        if (V.Texture != TextureIndex)
        {
            continue;
        }

        const float  Res   = float(V.Resolution);
        const float2 Texel = WorldXZ / V.TexelSize;
        const float2 Local = Texel - float2(V.Origin);
        const float  Edge  = min(min(Local.x, Local.y), min(Res - Local.x, Res - Local.y));
        Result.Valid = saturate((Edge - 1.0) / VIRTUAL_TEXTURE_EDGE_FADE_TEXELS);
        if (Result.Valid <= 0.0)
        {
            Result.Value = V.Clear;
            return Result;
        }

        // Wrapping filters across the image seam exactly where neighboring world texels meet.
        const float4 Stored = FTexture2D(V.Texture).SampleLevel(Samplers::LinearWrap, Texel / Res, 0.0);
        Result.Value = lerp(V.Clear, Stored, Result.Valid);
        return Result;
    }
    return Result;
}
