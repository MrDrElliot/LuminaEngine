#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/Sky.slang"

// Non-solid modes read SkyCube, which SkyCubeCapturePass fills once per frame.

struct FEnvPushConstants
{
    uint64_t     Env;             // FEnvironmentParams, see LoadBlock
    FTextureCube SkyCube;
    FTexture2D   Equirect;
    uint         EquirectWidth;   // equirect base width in texels; drives the anti-aliasing LOD
    uint         _Pad1;
};
FEnvPushConstants* Pass() { return GetPassArgs<FEnvPushConstants>(); }

// Uniform for the whole draw, so specializing it strips the three modes this pipeline is not.
[vk::constant_id(8)] const uint SPEC_SKY_MODE = SKY_MODE_RUNTIME;

struct FragmentInput  { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
struct FragmentOutput { float4 Color : SV_Target0; };

// GetInverseCameraProjection undoes the projection's Y flip; RayClip.z is arbitrary after normalizing.
float3 GetViewRay(float2 UV)
{
    float2 NDC      = UV * 2.0 - 1.0;
    float4 RayClip  = float4(NDC, 1.0, 1.0);
    float4 RayView  = mul(GetInverseCameraProjection(), RayClip);
    RayView.xyz    /= RayView.w;
    float3 RayWorld = mul(GetInverseCameraView(), float4(RayView.xyz, 0.0)).xyz;
    return normalize(RayWorld);
}

[shader("fragment")]
FragmentOutput FragmentMain(FragmentInput Input)
{
    FEnvironmentParams uEnvironment = LoadBlock<FEnvironmentParams>(Pass().Env);

    float3 Color;
    uint SkyMode = (SPEC_SKY_MODE == SKY_MODE_RUNTIME) ? asuint(uEnvironment.Misc.x) : SPEC_SKY_MODE;

    // Lighting overwrites covered pixels, but HDRI keeps them since its mip selection needs screen derivatives.
    if (SkyMode != SKY_MODE_HDRI && Scene().SceneDepth.IsValid()
        && Scene().SceneDepth.Load(int3(int2(Input.Position.xy), 0)).r > 0.0)
    {
        discard;
    }
    if (SkyMode == SKY_MODE_SOLID)
    {
        Color = uEnvironment.SolidSkyColor.rgb;
    }
    else
    {
        // HDRI reads its source equirect directly, since the cube is too coarse full-screen.
        float3 RayDir = GetViewRay(Input.UV);
        if (SkyMode == SKY_MODE_HDRI)
        {
            // Shared with EquirectToCubemap.slang so the visible sky lines up with the cube-derived IBL under any yaw.
            float2 UV = Sky_EquirectUV(RayDir, uEnvironment.HDRIParams.y, uEnvironment.HDRIParams.z);

            // RayDir has no equirect seam, so its derivatives give a clean angular footprint; SkyMode is uniform so they are safe.
            float3 dRdx = ddx(RayDir);
            float3 dRdy = ddy(RayDir);
            float pixelAngle      = max(length(dRdx), length(dRdy));   // radians per pixel, chord approximates angle
            float texelsPerRadian = float(Pass().EquirectWidth) * (1.0 / TWO_PI);
            float lod             = log2(max(pixelAngle * texelsPerRadian, 1.0));
            // An Inf texel from half-float overflow would blow out bloom and flash the screen white.
            Color = SanitizeRadiance(Pass().Equirect.SampleLevel(Samplers::LinearWrap, UV, lod).rgb);
            Color *= max(uEnvironment.HDRIParams.x, 0.0);
        }
        else
        {
            Color = Pass().SkyCube.SampleLevel(Samplers::LinearClamp, RayDir, 0.0).rgb;
        }

        // Sun, moon and stars are pixel-scale and blocky in a 256 cube, so they run per pixel.
        if (SkyMode == SKY_MODE_DYNAMIC)
        {
            float3 SunDir      = GetSkySunDirection();
            float  SunHeight   = SunDir.y;
            float  NightFactor = smoothstep(0.05, -0.15, SunHeight);
            float  UpMask      = smoothstep(-0.05, 0.1, RayDir.y);

            float3 Overlay = float3(0.0);

            // A single dot product gates the full Sky_RenderSun math.
            float DiscScale = uEnvironment.Misc.y;
            float DiscAngle = kSunBaseAngle * max(DiscScale, 0.0);
            if (dot(RayDir, SunDir) > cos(DiscAngle * 1.8))
            {
                float3 SunColor = uEnvironment.SunTint.rgb * uEnvironment.SunTint.w;
                Overlay += Sky_RenderSun(RayDir, SunDir, SunColor, DiscScale);
            }

            // Moon + stars only matter at night; stars need UpMask to stay out of the lit ground band.
            if (NightFactor > 0.0)
            {
                Overlay += Sky_RenderMoon(RayDir, SunDir, uEnvironment, NightFactor);
                if (UpMask > 0.0)
                {
                    Overlay += Sky_Stars(RayDir, uEnvironment, GetTime(), NightFactor) * UpMask;
                }
            }

            // Same exposure the cube was baked with, so overlays sit consistently against it.
            Color += Overlay * max(uEnvironment.Misc.z, 1e-3);
        }
    }

    FragmentOutput Output;
    Output.Color = float4(Color, 1.0);
    return Output;
}
