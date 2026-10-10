#pragma once
#include "SceneGlobals.slang"

// Aerial perspective between the camera and a world point, read from the LUT AerialPerspectivePass builds.
float3 AerialPerspective_Apply(float3 Color, float2 ScreenUV, float3 WorldPos, FTexture3D InScatter, FTexture3D Transmittance,
                               float Range, float Intensity)
{
    // Inverse of the LUT's squared slice spacing, since a linear lookup reads the wrong slice.
    float  RayDist = length(WorldPos - GetCameraPosition());
    float3 UVW     = float3(ScreenUV, saturate(sqrt(RayDist / max(Range, 1.0))));

    float3 Scatter   = InScatter.SampleLevel(Samplers::LinearClamp, UVW, 0.0).rgb;
    float3 Trans     = Transmittance.SampleLevel(Samplers::LinearClamp, UVW, 0.0).rgb;

    return lerp(Color, Color * Trans + Scatter, saturate(Intensity));
}
