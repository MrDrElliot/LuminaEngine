#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Sky.slang"

// The sky's own atmosphere integrated camera-outward per slice, so distance is tinted, not fogged.

struct FPushConstants
{
    uint64_t Env;             // FEnvironmentParams, see LoadBlock
    uint   InScatterUAV;      // bindless 3D UAV: rgb = in-scattered radiance
    uint   TransmittanceUAV;  // bindless 3D UAV: rgb = per-channel transmittance

    float3 SunDirection;      // surface -> sun
    float  Range;             // world distance the LUT's W axis spans
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define AP_TILE 8

[shader("compute")]
[numthreads(AP_TILE, AP_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    RWTexture3D<float4> uInScatter     = gRWTextures3D[Pass().InScatterUAV];
    RWTexture3D<float4> uTransmittance = gRWTextures3D[Pass().TransmittanceUAV];

    uint LutW, LutH, LutD;
    uInScatter.GetDimensions(LutW, LutH, LutD);
    if (DTid.x >= LutW || DTid.y >= LutH)
    {
        return;
    }

    FEnvironmentParams Env = LoadBlock<FEnvironmentParams>(Pass().Env);

    float2 UV      = (float2(DTid.xy) + 0.5) / float2(LutW, LutH);
    float2 NDC     = UV * 2.0 - 1.0;
    float4 ViewH   = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;
    float3 RayDir  = normalize(mul(GetInverseCameraView(), float4(ViewRay, 0.0)).xyz);

    float3 SunDir      = Pass().SunDirection;
    float3 SunColor    = Env.SunTint.rgb * Env.SunTint.w;
    float  MieG        = clamp(Env.Misc.w, -0.99, 0.99);
    float  SkyExposure = max(Env.Misc.z, 1e-3);

    float CosTheta = dot(RayDir, SunDir);
    float PhaseR   = Sky_RayleighPhase(CosTheta);
    float PhaseM   = Sky_MiePhase(CosTheta, MieG);

    // Planet-relative origin at the camera's own altitude, so haze thins as you climb.
    float3 Origin = float3(0.0, kPlanetR + max(GetCameraPosition().y, 0.0), 0.0);

    float3 SumR      = float3(0.0, 0.0, 0.0);
    float3 SumM      = float3(0.0, 0.0, 0.0);
    float3 PrimaryOD = float3(0.0, 0.0, 0.0);
    float  PrevT     = 0.0;

    [[loop]]
    for (uint Slice = 0u; Slice < LutD; ++Slice)
    {
        // Texel-centre distance, so the apply's linear filter lines up with what was integrated.
        float NextT  = ((float(Slice) + 0.5) / float(LutD)) * Pass().Range;
        float SegLen = max(NextT - PrevT, 0.0);

        float3 P = Origin + RayDir * (PrevT + SegLen * 0.5);
        PrevT = NextT;

        float Height = length(P) - kPlanetR;
        if (Height >= 0.0)
        {
            float DensR = exp(-Height / kRayleighH);
            float DensM = exp(-Height / kMieH);
            float DensO = Sky_OzoneDensity(Height);

            PrimaryOD += float3(DensR, DensM, DensO) * SegLen;

            // Matches Sky_Atmosphere's planet-shadow test so night haze does not stay lit.
            float2 SunGroundHit = Sky_RaySphereIntersect(P, SunDir, kPlanetR);
            if (SunGroundHit.x <= 0.0)
            {
                float2 LightHit = Sky_RaySphereIntersect(P, SunDir, kAtmosphereR);
                float3 LightOD  = Sky_OpticalDepth(P, SunDir, LightHit.y);
                float3 TotalOD  = PrimaryOD + LightOD;

                float3 Tr = exp(-(kRayleighCoeff * TotalOD.x
                                + kMieCoeff     * TotalOD.y * 1.1
                                + kOzoneCoeff   * TotalOD.z));

                SumR += DensR * Tr * SegLen;
                SumM += DensM * Tr * SegLen;
            }
        }

        float3 Rayleigh = kRayleighCoeff * PhaseR * SumR;
        float3 Mie      = kMieCoeff * PhaseM * SumM;

        const float kMsStrength = 0.9;
        const float kMsTint     = 0.15;
        float3 MsEnergy     = kRayleighCoeff * SumR;
        float  MsGrey       = Luminance(MsEnergy);
        float3 MultiScatter = lerp(float3(MsGrey), MsEnergy, kMsTint) * kMsStrength / PI;

        float3 InScatter = SunColor * (Rayleigh + Mie + MultiScatter) * SkyExposure;
        float3 Trans     = exp(-(kRayleighCoeff * PrimaryOD.x
                               + kMieCoeff     * PrimaryOD.y * 1.1
                               + kOzoneCoeff   * PrimaryOD.z));

        uint3 Coord = uint3(DTid.xy, Slice);
        uInScatter[Coord]     = float4(SanitizeRadiance(InScatter), 1.0);
        uTransmittance[Coord] = float4(saturate(Trans), 1.0);
    }
}
