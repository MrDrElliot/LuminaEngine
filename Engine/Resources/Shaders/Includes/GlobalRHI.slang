#pragma once
import RHI;

// The index and slot form of the heap, kept for the code the material graph emits and for Custom nodes.

float4 SampleTexture2D(uint TextureIndex, uint SamplerIndex, float2 UV)
{
    return FTexture2D(TextureIndex).Sample(FSampler(SamplerIndex), UV);
}

float4 SampleTexture2DGrad(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)
{
    return FTexture2D(TextureIndex).SampleGrad(FSampler(SamplerIndex), UV, Ddx, Ddy);
}

float4 SampleTexture2DLevel(uint TextureIndex, uint SamplerIndex, float2 UV, float Lod)
{
    return FTexture2D(TextureIndex).SampleLevel(FSampler(SamplerIndex), UV, Lod);
}

float4 SampleTexture2DArray(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice)
{
    return FTexture2DArray(TextureIndex).Sample(FSampler(SamplerIndex), float3(UV, (float)Slice));
}

float4 SampleTexture2DArrayLevel(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice, float Lod)
{
    return FTexture2DArray(TextureIndex).SampleLevel(FSampler(SamplerIndex), float3(UV, (float)Slice), Lod);
}

float4 SampleTexture2DArrayGrad(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice, float2 Ddx, float2 Ddy)
{
    return FTexture2DArray(TextureIndex).SampleGrad(FSampler(SamplerIndex), float3(UV, (float)Slice), Ddx, Ddy);
}

float4 LoadTexture2DArray(uint TextureIndex, int2 Texel, uint Slice, int Mip = 0)
{
    return FTexture2DArray(TextureIndex).Load(int4(Texel, (int)Slice, Mip));
}

float4 SampleTextureCube(uint TextureIndex, uint SamplerIndex, float3 Dir)
{
    return FTextureCube(TextureIndex).Sample(FSampler(SamplerIndex), Dir);
}

float4 SampleTextureCubeLevel(uint TextureIndex, uint SamplerIndex, float3 Dir, float Lod)
{
    return FTextureCube(TextureIndex).SampleLevel(FSampler(SamplerIndex), Dir, Lod);
}

float4 SampleTextureCubeArrayLevel(uint TextureIndex, uint SamplerIndex, float3 Dir, uint Slice, float Lod)
{
    return FTextureCubeArray(TextureIndex).SampleLevel(FSampler(SamplerIndex), Dir, Slice, Lod);
}

float4 SampleTexture3D(uint TextureIndex, uint SamplerIndex, float3 UVW)
{
    return FTexture3D(TextureIndex).Sample(FSampler(SamplerIndex), UVW);
}

float4 SampleTexture3DLevel(uint TextureIndex, uint SamplerIndex, float3 UVW, float Lod)
{
    return FTexture3D(TextureIndex).SampleLevel(FSampler(SamplerIndex), UVW, Lod);
}

float4 LoadTexture2D(uint TextureIndex, int2 Texel, int Mip = 0)
{
    return FTexture2D(TextureIndex).Load(Texel, Mip);
}

uint LoadTexture2DUInt(uint TextureIndex, int2 Texel, int Mip = 0)
{
    return FTexture2DUInt(TextureIndex).Load(Texel, Mip);
}

// UV has a top-left origin and depth lands on the far plane, matching the fullscreen quad convention.
float4 FullscreenTriangle(uint VertexID, out float2 UV)
{
    UV = float2((VertexID << 1) & 2, VertexID & 2);
    return float4(UV * 2.0 - 1.0, 1.0, 1.0);
}
