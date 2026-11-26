#include "Includes/SceneGlobals.slang"
#include "Includes/Sky.slang"

// The sky around the camera at its altitude, the sun's on the left half and the moon's on the right, each times its light.

// Matches FSkyViewLUTArgs in SceneRenderer_Environment.cpp.
struct FSkyViewLUTArgs
{
    Ptr<FEnvironmentParams> Env;
    FRWTexture2D            Out;
    FTexture2D              Transmittance;
    float3                  SunDirection;
    FTexture2D              MultiScatter;
};
FSkyViewLUTArgs* Pass() { return GetPassArgs<FSkyViewLUTArgs>(); }

static const int kSkyViewSteps = 30;

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= SKY_VIEW_LUT_WIDTH * 2u || DTid.y >= SKY_VIEW_LUT_HEIGHT)
    {
        return;
    }

    FEnvironmentParams Env = *Pass().Env;
    const uint Half = DTid.x / SKY_VIEW_LUT_WIDTH;

    float3 SunDir   = normalize(Pass().SunDirection);
    float3 LightDir = Half == 0u ? SunDir : Sky_MoonDirection(Env, SunDir);
    float3 Illuminance = Env.SunTint.rgb * (Half == 0u ? kSolarIrradiance : Env.AtmosphereParams.z);
    if (all(Illuminance <= 0.0))
    {
        Pass().Out[DTid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float  Radius = Sky_ViewRadius(GetCameraPosition().y);
    float2 Unit   = float2(float(DTid.x - Half * SKY_VIEW_LUT_WIDTH), float(DTid.y)) / (kSkyViewLUTSize - 1.0);
    float  CosViewZenith;
    float  CosLightView;
    Sky_SkyViewParams(Unit, Radius, CosViewZenith, CosLightView);

    // A frame where the light's azimuth is +X, which is all the LUT's azimuth axis measures from.
    float  CosLightZenith = clamp(LightDir.y, -1.0, 1.0);
    float3 LocalLight     = float3(sqrt(saturate(1.0 - CosLightZenith * CosLightZenith)), CosLightZenith, 0.0);
    float  SinViewZenith  = sqrt(saturate(1.0 - CosViewZenith * CosViewZenith));
    float3 LocalView      = float3(CosLightView * SinViewZenith, CosViewZenith, sqrt(saturate(1.0 - CosLightView * CosLightView)) * SinViewZenith);

    FSkyIntegral S = Sky_Integrate(Pass().Transmittance, Pass().MultiScatter, true, float3(0.0, Radius, 0.0), LocalView, LocalLight,
                                   kSkyViewSteps, Env.AtmosphereParams.x, clamp(Env.Misc.w, -0.99, 0.99), false,
                                   Env.GroundColor.rgb * kGroundRadianceScale);

    Pass().Out[DTid.xy] = float4(SanitizeRadiance(S.Luminance * Illuminance), 1.0);
}
