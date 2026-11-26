#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/Sky.slang"

// The dynamic sky reads the sky view LUT per pixel, and the gradient reads the cube SkyCubeCapturePass fills.

// Matches FEnvPushConstants in SceneRenderer_Environment.cpp.
struct FEnvPushConstants
{
    Ptr<FEnvironmentParams> Env;
    FTextureCube SkyCube;
    FTexture2D   Equirect;
    float3       SunDirection;
    uint         EquirectWidth;   // equirect base width in texels; drives the anti-aliasing LOD
    FTexture2D   Transmittance;
    FTexture2D   SkyView;
    uint         _Pad0;
    uint         _Pad1;
};
FEnvPushConstants* Pass() { return GetPassArgs<FEnvPushConstants>(); }

// Uniform for the whole draw, so specializing it strips the three modes this pipeline is not.
[vk::constant_id(8)] const uint SPEC_SKY_MODE = SKY_MODE_RUNTIME;

struct FragmentInput  { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
struct FragmentOutput { float4 Color : SV_Target0; };

[shader("fragment")]
FragmentOutput FragmentMain(FragmentInput Input)
{
    FEnvironmentParams uEnvironment = *Pass().Env;

    float3 Color;
    uint SkyMode = (SPEC_SKY_MODE == SKY_MODE_RUNTIME) ? asuint(uEnvironment.Misc.x) : SPEC_SKY_MODE;

    // The pass's equal test against the far plane already rejects every covered pixel before this runs.
    if (SkyMode == SKY_MODE_SOLID)
    {
        Color = uEnvironment.SolidSkyColor.rgb;
    }
    else
    {
        float3 RayDir = GetCameraWorldRayDir(Input.UV);

        // RayDir has no seam, so its derivatives give a clean angular footprint, and SkyMode is uniform so they are safe.
        float3 dRdx       = ddx(RayDir);
        float3 dRdy       = ddy(RayDir);
        float  PixelAngle = max(length(dRdx), length(dRdy));

        if (SkyMode == SKY_MODE_HDRI)
        {
            // Shared with EquirectToCubemap.slang so the visible sky lines up with the cube-derived IBL under any yaw.
            float2 UV = Sky_EquirectUV(RayDir, uEnvironment.HDRIParams.y, uEnvironment.HDRIParams.z);
            float texelsPerRadian = float(Pass().EquirectWidth) * (1.0 / TWO_PI);
            float lod             = log2(max(PixelAngle * texelsPerRadian, 1.0));
            // An Inf texel from half-float overflow would blow out bloom and flash the screen white.
            Color = SanitizeRadiance(Pass().Equirect.SampleLevel(Samplers::LinearWrap, UV, lod).rgb);
            Color *= max(uEnvironment.HDRIParams.x, 0.0);
        }
        else if (SkyMode == SKY_MODE_DYNAMIC)
        {
            float3 SunDir      = normalize(Pass().SunDirection);
            float3 MoonDir     = Sky_MoonDirection(uEnvironment, SunDir);
            float  Radius      = Sky_ViewRadius(GetCameraPosition().y);
            float  Exposure    = max(uEnvironment.Misc.z, 1e-3);
            FTexture2D TransmittanceLut = Pass().Transmittance;

            Color = Sky_Dynamic(uEnvironment, Pass().SkyView, TransmittanceLut, Radius, SunDir, MoonDir, RayDir);

            // Sun, moon and stars are pixel-scale and would smear in any LUT, so they run per pixel.
            float3 ViewTransmittance = Sky_SampleTransmittance(TransmittanceLut, Radius, RayDir.y);
            float3 Overlay = Sky_RenderSun(RayDir, SunDir, uEnvironment.SunTint.rgb * uEnvironment.SunTint.w, uEnvironment.Misc.y,
                                           ViewTransmittance, PixelAngle);

            float NightFactor = Sky_NightFactor(SunDir);
            Overlay += Sky_RenderMoon(RayDir, SunDir, MoonDir, uEnvironment, ViewTransmittance, PixelAngle, NightFactor);
            if (NightFactor > 0.0)
            {
                // Stars stay out of the ground band below the horizon.
                float UpMask = smoothstep(-0.05, 0.1, RayDir.y);
                if (UpMask > 0.0)
                {
                    Overlay += Sky_Stars(RayDir, uEnvironment, GetTime(), NightFactor, ViewTransmittance) * UpMask;
                }
            }

            Color += Overlay * Exposure;
        }
        else
        {
            Color = Pass().SkyCube.SampleLevel(Samplers::LinearClamp, RayDir, 0.0).rgb;
        }
    }

    FragmentOutput Output;
    Output.Color = float4(Color, 1.0);
    return Output;
}
