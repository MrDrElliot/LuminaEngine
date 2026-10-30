#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Sky.slang"
#include "Includes/CloudCommon.slang"
#include "Includes/CloudRay.slang"

// Shell march between two spheres about the same planet the atmosphere uses, so the horizon curves.

struct FPushConstants
{
    FRWTexture2D Scatter;      // half-res rgb = scattered radiance, a = transmittance
    FTexture2D   Depth;        // scene depth, clamps the march
    FTexture3D   Noise;        // the baked shape and erosion volume
    uint         ScreenW;

    uint   ScreenH;
    uint   MarchSteps;
    uint   LightSteps;
    float  MaxDistance;

    float3 SunDirection;    // surface -> sun
    float  LayerBottom;     // altitude above y = 0

    float3 SunColor;
    float  LayerTop;

    float3 AmbientColor;
    float  Coverage;

    float  Density;
    float  ShapeScale;
    float  DetailScale;
    float  DetailStrength;

    float  Billow;
    float  ForwardScattering;
    float  BackScattering;
    float  PowderStrength;

    float2 WindOffset;      // baked from direction * speed * time
    float2 DetailWindOffset;

    FRWTexture2D CloudDepth;   // raw depth the march was clamped to, 0 where it saw sky
    uint         Stride;       // 2 marches one texel of each 2x2 block, which the resolve then fills around
    uint         OffsetX;
    uint         OffsetY;

    uint         FrameIndex;   // turns the march jitter every frame, so the resolve integrates it
    FTexture2D   Weather;
    uint         _Pad1;
    uint         _Pad2;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// A forward lobe alone leaves cloud away from the sun unnaturally flat.
float Cloud_Phase(float CosTheta, float G1, float G2)
{
    return lerp(Sky_MiePhase(CosTheta, G1), Sky_MiePhase(CosTheta, G2), 0.35);
}

FCloudMedium Cloud_Medium()
{
    FCloudMedium M;
    M.Noise            = Pass().Noise;
    M.ShapeScale       = Pass().ShapeScale;
    M.DetailScale      = Pass().DetailScale;
    M.DetailStrength   = Pass().DetailStrength;
    M.Billow           = Pass().Billow;
    M.Coverage         = Pass().Coverage;
    M.Density          = Pass().Density;
    M.LayerBottom      = Pass().LayerBottom;
    M.LayerTop         = Pass().LayerTop;
    M.Weather          = Pass().Weather;
    M.WindOffset       = Pass().WindOffset;
    M.DetailWindOffset = Pass().DetailWindOffset;
    M._Pad1            = float2(0.0);
    return M;
}

float Cloud_SampleDensity(float3 WorldPos, float HeightFrac, bool bCheap)
{
    return Cloud_SampleDensityM(Cloud_Medium(), WorldPos, HeightFrac, bCheap);
}

float Cloud_SampleDensityEnvelope(float3 WorldPos, float HeightFrac, out float Envelope)
{
    return Cloud_SampleDensityEnvelopeM(Cloud_Medium(), WorldPos, HeightFrac, false, Envelope);
}

// A step wider than a small cumulus lets neighboring pixels disagree on whether it is there at all.
static const float kMaxStepLength = 150.0;
static const uint  kMaxStepScale  = 4u;

static const float kEmptyStepScale = 3.0;
static const float kMaxEmptyStride = 300.0;
static const uint  kEntryFineSteps = 3u;

// Turned by the golden ratio each frame, since one fixed offset per pixel samples a cloud at the same slices forever and the resolve sees them as slats.
float Cloud_MarchJitter(float2 PixelPos, uint FrameIndex)
{
    return frac(InterleavedGradientNoise(PixelPos) + float(FrameIndex & 1023u) * 0.61803399);
}

// Light that scattered more than once, approximated by octaves each weaker, softer and more forward, so dense cores glow instead of going flat.
static const uint  kScatterOctaves     = 3u;
static const float kOctaveAttenuation  = 0.5;
static const float kOctaveExtinction   = 0.5;
static const float kOctaveEccentricity = 0.5;

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint2 Size = uint2(Pass().ScreenW, Pass().ScreenH);
    int2  P    = int2(DTid.xy * Pass().Stride + uint2(Pass().OffsetX, Pass().OffsetY));
    if (P.x >= int(Size.x) || P.y >= int(Size.y))
    {
        return;
    }

    const FCloudRay Ray = Cloud_SetupRay(Pass().Depth, P, Size, Pass().LayerBottom, Pass().LayerTop, Pass().MaxDistance);
    Pass().CloudDepth[P] = float4(Ray.RawDepth, 0.0, 0.0, 0.0);
    if (Ray.End <= Ray.Start)
    {
        Pass().Scatter[P] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float3 RayDir = Ray.Dir;
    float3 CamPos = GetCameraPosition();
    float3 Origin = Ray.Origin;
    float  InnerR = Ray.InnerR;
    float  OuterR = Ray.OuterR;
    float  Start  = Ray.Start;
    float  End    = Ray.End;

    // Grazing rays cross tens of kilometers of shell, so they buy more steps rather than outgrowing a cumulus.
    uint  MinSteps = max(Pass().MarchSteps, 16u);
    uint  Steps    = clamp(uint(ceil((End - Start) / kMaxStepLength)), MinSteps, MinSteps * kMaxStepScale);
    float BaseStep = (End - Start) / float(Steps);

    float LayerThickness = max(OuterR - InnerR, 1.0);
    float CosTheta       = dot(RayDir, Pass().SunDirection);

    // The phase per octave is fixed along the ray, so it is weighed once here rather than per sample.
    float OctavePhase[kScatterOctaves];
    [unroll]
    for (uint o = 0u; o < kScatterOctaves; ++o)
    {
        const float Eccentricity = pow(kOctaveEccentricity, float(o));
        OctavePhase[o] = pow(kOctaveAttenuation, float(o))
                       * Cloud_Phase(CosTheta, Pass().ForwardScattering * Eccentricity, Pass().BackScattering * Eccentricity);
    }

    float3 Scattered     = float3(0.0, 0.0, 0.0);
    float  Transmittance = 1.0;

    // Clear air between clouds is cheap, so stride through it in whole fine steps, which keeps every sample on one jittered lattice.
    float EmptyStep = BaseStep * clamp(floor(kMaxEmptyStride / BaseStep), 1.0, kEmptyStepScale);

    // Within one fine step, since the empty strides are whole fine steps and a wider jitter lets a stride skip a small puff on some frames only.
    float T        = Start + Cloud_MarchJitter(float2(P), Pass().FrameIndex) * BaseStep;
    bool  bStrode  = false;
    uint  FineLeft = 0u;

    [loop]
    for (uint i = 0u; i < Steps * 2u && T < End; ++i)
    {
        if (Transmittance < 0.01)
        {
            break;
        }

        float3 SamplePos = Origin + RayDir * T;

        float Altitude   = length(SamplePos) - kPlanetR;
        float HeightFrac = saturate((Altitude - Pass().LayerBottom) / LayerThickness);

        float3 WorldSample = float3(CamPos.x, 0.0, CamPos.z) + RayDir * T;
        WorldSample.y = Altitude;

        // Strides only through clear air, since a stride across a cloud's eroded holes skips its thin parts on whole rows of pixels.
        float Envelope;
        float Density = Cloud_SampleDensityEnvelope(WorldSample, HeightFrac, Envelope);
        if (Envelope <= 0.0)
        {
            float Advance = FineLeft > 0u ? BaseStep : EmptyStep;
            FineLeft = FineLeft > 0u ? FineLeft - 1u : 0u;
            bStrode  = Advance > BaseStep;
            T += Advance;
            continue;
        }

        // The stride may have landed past the leading edge, so back up once and walk in at the fine step.
        if (bStrode)
        {
            T -= EmptyStep - BaseStep;
            bStrode  = false;
            FineLeft = kEntryFineSteps;
            continue;
        }

        T += BaseStep;
        if (Density <= 0.0)
        {
            continue;
        }

        // Widening light steps let the far samples cover the cloud's bulk cheaply.
        uint  LSteps   = max(Pass().LightSteps, 1u);
        float LStepLen = LayerThickness * 0.5 / float(LSteps);
        float SunDepth = 0.0;
        float LT       = 0.0;

        [loop]
        for (uint l = 0u; l < LSteps; ++l)
        {
            float SegmentLen = LStepLen;
            LT += SegmentLen;
            LStepLen *= 1.5;

            float3 LP  = SamplePos + Pass().SunDirection * LT;
            float  LAl = length(LP) - kPlanetR;
            float  LHF = saturate((LAl - Pass().LayerBottom) / LayerThickness);
            if (LHF <= 0.0 || LHF >= 1.0)
            {
                break;
            }

            float3 LWorld = WorldSample + Pass().SunDirection * LT;
            LWorld.y = LAl;

            SunDepth += Cloud_SampleDensity(LWorld, LHF, true) * SegmentLen;
        }

        float SunScatter = 0.0;
        [unroll]
        for (uint o = 0u; o < kScatterOctaves; ++o)
        {
            SunScatter += OctavePhase[o] * exp(-SunDepth * pow(kOctaveExtinction, float(o)));
        }

        // Powder, since thin sunward edges scatter out before they scatter back and read dark.
        float Powder = 1.0 - exp(-Density * BaseStep * 2.0);
        float Lit    = lerp(1.0, Powder, Pass().PowderStrength);

        float3 SunTerm     = Pass().SunColor * SunScatter * Lit;
        float3 AmbientTerm = Pass().AmbientColor * lerp(0.35, 1.0, HeightFrac);

        float StepTrans = exp(-Density * BaseStep);

        // Analytic segment integral rather than a Riemann sum, which keeps energy at coarse steps.
        float3 StepScatter = (SunTerm + AmbientTerm) * (1.0 - StepTrans);

        Scattered     += StepScatter * Transmittance;
        Transmittance *= StepTrans;
    }

    // Distant cloud thins into the horizon haze, rather than ending on the hard line where the march is clamped.
    float DistanceFade = 1.0 - smoothstep(Pass().MaxDistance * 0.4, Pass().MaxDistance, Start);
    Scattered     *= DistanceFade;
    Transmittance  = lerp(1.0, Transmittance, DistanceFade);

    Pass().Scatter[P] = float4(SanitizeRadiance(Scattered), Transmittance);
}
