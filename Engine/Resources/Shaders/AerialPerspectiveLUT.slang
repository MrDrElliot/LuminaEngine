#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Sky.slang"

// The sky's own atmosphere integrated outward from the camera per slice, so distance is tinted the way the horizon is.

// Matches FAerialLUTPushConstants in SceneRenderer_Atmosphere.cpp.
struct FPushConstants
{
    Ptr<FEnvironmentParams> Env;
    FRWTexture3D InScatter;       // rgb = in-scattered radiance
    FRWTexture3D Transmittance;   // rgb = per-channel transmittance

    float3       SunDirection;    // surface -> sun
    float        Range;           // world distance the LUT's W axis spans

    FTexture2D   TransmittanceLut;
    FTexture2D   MultiScatterLut;
    uint         _Pad0;
    uint         _Pad1;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define AP_TILE 8

[shader("compute")]
[numthreads(AP_TILE, AP_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FRWTexture3D uInScatter     = Pass().InScatter;
    FRWTexture3D uTransmittance = Pass().Transmittance;
    FTexture2D   TransmittanceLut = Pass().TransmittanceLut;
    FTexture2D   MultiScatterLut  = Pass().MultiScatterLut;

    uint LutW, LutH, LutD;
    uInScatter.GetDimensions(LutW, LutH, LutD);
    if (DTid.x >= LutW || DTid.y >= LutH)
    {
        return;
    }

    FEnvironmentParams Env = *Pass().Env;

    float2 UV     = (float2(DTid.xy) + 0.5) / float2(LutW, LutH);
    float3 RayDir = GetCameraWorldRayDir(UV);

    float3 SunDir  = normalize(Pass().SunDirection);
    float3 MoonDir = Sky_MoonDirection(Env, SunDir);
    float  MieG    = clamp(Env.Misc.w, -0.99, 0.99);
    float  Aerosol = Env.AtmosphereParams.x;

    // The same lights the sky view LUT was built with, or the haze drifts off the horizon it should melt into.
    float3 SunIlluminance  = Env.SunTint.rgb * kSolarIrradiance;
    float3 MoonIlluminance = Env.SunTint.rgb * Env.AtmosphereParams.z;
    bool   bMoon           = any(MoonIlluminance > 0.0);

    float SunCos   = dot(RayDir, SunDir);
    float SunPhaseR = Sky_RayleighPhase(SunCos);
    float SunPhaseM = Sky_MiePhaseCS(SunCos, MieG);
    float MoonCos   = dot(RayDir, MoonDir);
    float MoonPhaseR = Sky_RayleighPhase(MoonCos);
    float MoonPhaseM = Sky_MiePhaseCS(MoonCos, MieG);

    float3 Origin = float3(0.0, Sky_ViewRadius(GetCameraPosition().y), 0.0);
    float  Exposure = max(Env.Misc.z, 1e-3);

    float3 Luminance     = float3(0.0, 0.0, 0.0);
    float3 Throughput    = float3(1.0, 1.0, 1.0);
    float  PrevT         = 0.0;

    [[loop]]
    for (uint Slice = 0u; Slice < LutD; ++Slice)
    {
        // Squared spacing, since even slices put the nearest sample 125m out and haze a whole street scene.
        float Frac   = (float(Slice) + 0.5) / float(LutD);
        float NextT  = Pass().Range * Frac * Frac;
        float Dt     = max(NextT - PrevT, 0.0);
        float3 P     = Origin + RayDir * (PrevT + Dt * 0.5);
        PrevT = NextT;

        // Clamped to the surface, not skipped, since skipping leaves a hard seam along the horizon.
        float  Radius = max(length(P), kPlanetR);
        float3 Up     = normalize(P);
        FSkyMedium M  = Sky_Medium(Radius - kPlanetR, Aerosol);
        float3 Scattering = M.ScatteringR + M.ScatteringM;
        float3 StepTrans  = exp(-M.Extinction * Dt);
        float3 InvExt     = 1.0 / max(M.Extinction, float3(1e-12, 1e-12, 1e-12));

        float  SunZenith    = dot(Up, SunDir);
        float  SunShadow    = (Sky_RaySphereIntersect(P + Up * 10.0, SunDir, kPlanetR).x > 0.0) ? 0.0 : 1.0;
        float3 S = (SunShadow * Sky_SampleTransmittance(TransmittanceLut, Radius, SunZenith) * (M.ScatteringR * SunPhaseR + M.ScatteringM * SunPhaseM)
                 + Sky_SampleMultiScatter(MultiScatterLut, Radius, SunZenith) * Scattering) * SunIlluminance;

        if (bMoon)
        {
            float MoonZenith = dot(Up, MoonDir);
            float MoonShadow = (Sky_RaySphereIntersect(P + Up * 10.0, MoonDir, kPlanetR).x > 0.0) ? 0.0 : 1.0;
            S += (MoonShadow * Sky_SampleTransmittance(TransmittanceLut, Radius, MoonZenith) * (M.ScatteringR * MoonPhaseR + M.ScatteringM * MoonPhaseM)
                + Sky_SampleMultiScatter(MultiScatterLut, Radius, MoonZenith) * Scattering) * MoonIlluminance;
        }

        Luminance  += Throughput * (S - S * StepTrans) * InvExt;
        Throughput *= StepTrans;

        uint3 Coord = uint3(DTid.xy, Slice);
        uInScatter[Coord]     = float4(SanitizeRadiance(Luminance * Exposure), 1.0);
        uTransmittance[Coord] = float4(saturate(Throughput), 1.0);
    }
}
