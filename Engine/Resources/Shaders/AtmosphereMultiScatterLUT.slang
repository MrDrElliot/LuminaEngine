#include "Includes/SceneGlobals.slang"
#include "Includes/Sky.slang"

// Hillaire 2020 on the left half, and on the right the skylight on level ground that lights the planet below the horizon.

// Matches FMultiScatterLUTArgs in SceneRenderer_Environment.cpp.
struct FMultiScatterLUTArgs
{
    FRWTexture2D Out;
    FTexture2D   Transmittance;
    float        Aerosol;
    uint         _Pad0;
};
FMultiScatterLUTArgs* Pass() { return GetPassArgs<FMultiScatterLUTArgs>(); }

#define MS_DIRECTIONS_SQRT 8u
#define MS_DIRECTIONS      (MS_DIRECTIONS_SQRT * MS_DIRECTIONS_SQRT)

static const int kMultiScatterSteps = 20;

groupshared float3 GSLuminance[MS_DIRECTIONS];
groupshared float3 GSTransfer[MS_DIRECTIONS];
groupshared float3 GSSkylight[MS_DIRECTIONS];

// One group per texel, one thread per direction over the whole sphere.
[shader("compute")]
[numthreads(1, 1, MS_DIRECTIONS)]
void ComputeMain(uint3 Gid : SV_GroupID, uint GI : SV_GroupIndex)
{
    float2 Unit           = float2(Gid.xy) / (kMultiScatterLUTSize - 1.0);
    float  CosLightZenith = Unit.x * 2.0 - 1.0;
    float  Radius         = clamp(kPlanetR + Unit.y * (kAtmosphereR - kPlanetR), kPlanetR + 1.0, kAtmosphereR - 1.0);
    float3 Origin         = float3(0.0, Radius, 0.0);
    float3 LightDir       = float3(0.0, CosLightZenith, sqrt(saturate(1.0 - CosLightZenith * CosLightZenith)));

    float  Azimuth = TWO_PI * (float(GI / MS_DIRECTIONS_SQRT) + 0.5) / float(MS_DIRECTIONS_SQRT);
    float  CosPolar = 1.0 - 2.0 * (float(GI % MS_DIRECTIONS_SQRT) + 0.5) / float(MS_DIRECTIONS_SQRT);
    float  SinPolar = sqrt(saturate(1.0 - CosPolar * CosPolar));
    float3 Dir      = float3(cos(Azimuth) * SinPolar, CosPolar, sin(Azimuth) * SinPolar);

    FTexture2D Transmittance = Pass().Transmittance;
    FSkyIntegral S = Sky_Integrate(Transmittance, Transmittance, false, Origin, Dir, LightDir, kMultiScatterSteps,
                                   Pass().Aerosol, 0.0, true, float3(kPlanetAlbedo, kPlanetAlbedo, kPlanetAlbedo) * INV_PI);

    GSLuminance[GI] = S.Luminance;
    GSTransfer[GI]  = S.MultiScatterAs1;
    GSSkylight[GI]  = S.Luminance * max(CosPolar, 0.0);
    GroupMemoryBarrierWithGroupSync();

    [[unroll]]
    for (uint Stride = MS_DIRECTIONS / 2u; Stride > 0u; Stride >>= 1u)
    {
        if (GI < Stride)
        {
            GSLuminance[GI] += GSLuminance[GI + Stride];
            GSTransfer[GI]  += GSTransfer[GI + Stride];
            GSSkylight[GI]  += GSSkylight[GI + Stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (GI == 0u)
    {
        // The sphere's solid angle and the isotropic phase cancel, which leaves a plain mean over the directions.
        float3 SecondOrder = GSLuminance[0] / float(MS_DIRECTIONS);
        float3 Transfer    = GSTransfer[0] / float(MS_DIRECTIONS);
        // The geometric series of every further bounce, each scattering the same fraction of the last.
        float3 Series = 1.0 / max(1.0 - Transfer, float3(1e-3, 1e-3, 1e-3));
        Pass().Out[Gid.xy] = float4(SecondOrder * Series, 1.0);

        // Each direction stands for a 4pi/N patch of sphere, and the cosine weights it onto a level surface.
        float3 Skylight = GSSkylight[0] * (4.0 * PI / float(MS_DIRECTIONS)) * Series;
        Pass().Out[Gid.xy + uint2(SKY_MULTISCATTER_LUT_SIZE, 0u)] = float4(Skylight, 1.0);
    }
}
