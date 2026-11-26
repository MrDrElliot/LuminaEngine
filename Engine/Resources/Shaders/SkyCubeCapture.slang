#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Sky.slang"

// The sky for IBL, so the sun and moon discs and the stars stay out; the directional light already carries them.

// Matches FSkyCapturePC in SceneRenderer_Environment.cpp.
struct FPushConstants
{
    Ptr<FEnvironmentParams> Env;
    FRWTexture2DArray       SkyCube;
    FTexture2D              Transmittance;
    float3                  SunDirection;
    FTexture2D              SkyView;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define SKYCAP_TILE 8

[shader("compute")]
[numthreads(SKYCAP_TILE, SKYCAP_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FEnvironmentParams Env      = *Pass().Env;
    FRWTexture2DArray  uSkyCube = Pass().SkyCube;

    uint Width, Height, Layers;
    uSkyCube.GetDimensions(Width, Height, Layers);

    if (DTid.x >= Width || DTid.y >= Height || DTid.z >= Layers)
    {
        return;
    }

    float2 UV     = (float2(DTid.xy) + 0.5) / float2(Width, Height);
    float3 RayDir = CubeFaceDirection(DTid.z, UV);
    float3 SunDir = normalize(Pass().SunDirection);
    float  Radius = Sky_ViewRadius(GetCameraPosition().y);

    float3 Color;
    uint Mode = asuint(Env.Misc.x);
    if (Mode == SKY_MODE_SOLID)
    {
        Color = Sky_Solid(Env);
    }
    else if (Mode == SKY_MODE_GRADIENT)
    {
        // A texel's angular width, so the baked disc is as soft as the cube can hold and no softer.
        float TexelAngle = 2.0 / float(Width);
        Color = Sky_Gradient(Env, SunDir, RayDir, Sky_SampleTransmittance(Pass().Transmittance, Radius, RayDir.y), TexelAngle);
    }
    else
    {
        Color = Sky_Dynamic(Env, Pass().SkyView, Pass().Transmittance, Radius, SunDir, Sky_MoonDirection(Env, SunDir), RayDir);
    }

    uSkyCube[DTid] = float4(SanitizeRadiance(Color), 1.0);
}
