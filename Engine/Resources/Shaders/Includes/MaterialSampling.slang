#pragma once
#include "GlobalRHI.slang"

// The sample calls the material compiler emits. The includer defines GetMaterialMipBias() before this file.

// Which sampling mode is right depends on the lane, not the material, since a deferred lane has no quad to derive from.
#if defined(SHADING_COMPUTE) || defined(SHADING_EXPLICIT_GRADIENTS)
float4 SampleTexture2DAuto(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)
{
    const float GradScale = exp2(GetMaterialMipBias());
    return SampleTexture2DGrad(TextureIndex, SamplerIndex, UV, Ddx * GradScale, Ddy * GradScale);
}

float4 SampleTexture2DArrayAuto(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice, float2 Ddx, float2 Ddy)
{
    const float GradScale = exp2(GetMaterialMipBias());
    return SampleTexture2DArrayGrad(TextureIndex, SamplerIndex, UV, Slice, Ddx * GradScale, Ddy * GradScale);
}
#else
float4 SampleTexture2DAuto(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)
{
    return FTexture2D(TextureIndex).SampleBias(FSampler(SamplerIndex), UV, GetMaterialMipBias());
}

float4 SampleTexture2DArrayAuto(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice, float2 Ddx, float2 Ddy)
{
    return FTexture2DArray(TextureIndex).SampleBias(FSampler(SamplerIndex), float3(UV, (float)Slice), GetMaterialMipBias());
}
#endif
