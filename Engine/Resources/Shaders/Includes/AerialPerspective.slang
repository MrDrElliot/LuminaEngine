#pragma once
#include "SceneGlobals.slang"

// Aerial perspective between the camera and a world point, read from the LUT AerialPerspectivePass builds.
float3 AerialPerspective_Apply(float3 Color, float2 ScreenUV, float3 WorldPos, uint InScatterIndex, uint TransmittanceIndex,
                               float Range, float Intensity)
{
    // Inverse of the LUT's squared slice spacing, since a linear lookup reads the wrong slice.
    float  RayDist = length(WorldPos - GetCameraPosition());
    float3 UVW     = float3(ScreenUV, saturate(sqrt(RayDist / max(Range, 1.0))));

    float3 InScatter = gTextures3D[NonUniformResourceIndex(InScatterIndex)].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], UVW, 0.0).rgb;
    float3 Trans     = gTextures3D[NonUniformResourceIndex(TransmittanceIndex)].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], UVW, 0.0).rgb;

    return lerp(Color, Color * Trans + InScatter, saturate(Intensity));
}
