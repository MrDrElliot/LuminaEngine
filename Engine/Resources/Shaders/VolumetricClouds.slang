#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Sky.slang"

// Shell march between two spheres about the same planet the atmosphere uses, so the horizon curves.

struct FPushConstants
{
    uint   ScatterUAV;      // bindless 2D UAV: half-res rgb = scattered radiance, a = transmittance
    uint   DepthIndex;      // bindless 2D SRV: scene depth, clamps the march
    uint   NoiseIndex;      // bindless 3D SRV: the baked shape/erosion volume
    uint   ScreenW;

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
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float Cloud_Remap(float V, float InMin, float InMax, float OutMin, float OutMax)
{
    return OutMin + (V - InMin) * (OutMax - OutMin) / max(InMax - InMin, 1e-5);
}

// Two lobes: a forward one alone leaves cloud away from the sun unnaturally flat.
float Cloud_Phase(float CosTheta, float G1, float G2)
{
    return lerp(Sky_MiePhase(CosTheta, G1), Sky_MiePhase(CosTheta, G2), 0.35);
}

// Thin bases should not lose their vertical shape the way a full billow profile would.
float Cloud_HeightGradient(float H, float Billow)
{
    float Base = saturate(Cloud_Remap(H, 0.0, 0.15, 0.0, 1.0));
    float Top  = saturate(Cloud_Remap(H, 0.45, 1.0, 1.0, 0.0));
    float Flat = Base * Top;

    float Round = saturate(Cloud_Remap(H, 0.0, 0.25, 0.0, 1.0))
                * saturate(Cloud_Remap(H, 0.7, 1.0, 1.0, 0.0));
    return lerp(Flat, Round, Billow);
}

float Cloud_SampleDensity(float3 WorldPos, float HeightFrac, bool bCheap)
{
    float3 ShapeUVW = (WorldPos + float3(Pass().WindOffset.x, 0.0, Pass().WindOffset.y))
                    / max(Pass().ShapeScale, 1.0);

    float4 Shape = SampleTexture3D(Pass().NoiseIndex, SAMPLER_LINEAR_WRAP, ShapeUVW);

    // The Worley octaves ride on the base as a noise floor, which is what carves the billows.
    float LowFreq = Shape.g * 0.625 + Shape.b * 0.25 + Shape.a * 0.125;
    float Base    = saturate(Cloud_Remap(Shape.r, LowFreq - 1.0, 1.0, 0.0, 1.0));

    Base *= Cloud_HeightGradient(HeightFrac, Pass().Billow);

    // Coverage as the remap floor erodes inward, so puffs shrink rather than uniformly fading out.
    float Cov     = saturate(Pass().Coverage);
    float Density = saturate(Cloud_Remap(Base, 1.0 - Cov, 1.0, 0.0, 1.0));
    if (Density <= 0.0 || bCheap)
    {
        return Density * Pass().Density;
    }

    float3 DetailUVW = (WorldPos + float3(Pass().DetailWindOffset.x, 0.0, Pass().DetailWindOffset.y))
                     / max(Pass().DetailScale, 1.0);

    float4 Detail    = SampleTexture3D(Pass().NoiseIndex, SAMPLER_LINEAR_WRAP, DetailUVW);
    float  DetailFBM = Detail.g * 0.625 + Detail.b * 0.25 + Detail.a * 0.125;

    // Wispy at the base and billowed aloft, which is the direction real erosion runs.
    float Erosion = lerp(1.0 - DetailFBM, DetailFBM, saturate(HeightFrac * 2.0));
    Density = saturate(Cloud_Remap(Density, Erosion * Pass().DetailStrength, 1.0, 0.0, 1.0));

    return Density * Pass().Density;
}

// Interleaved gradient noise: offsetting the first sample hides the march's step banding.
float Cloud_IGN(float2 PixelPos)
{
    return InterleavedGradientNoise(PixelPos);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint2 Size = uint2(Pass().ScreenW, Pass().ScreenH);
    if (DTid.x >= Size.x || DTid.y >= Size.y)
    {
        return;
    }

    int2   P  = int2(DTid.xy);
    float2 UV = (float2(DTid.xy) + 0.5) / float2(Size);

    gRWTextures2D[Pass().ScatterUAV][P] = float4(0.0, 0.0, 0.0, 1.0);

    float2 NDC     = UV * 2.0 - 1.0;
    float4 ViewH   = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;
    float3 RayDir  = normalize(mul(GetInverseCameraView(), float4(ViewRay, 0.0)).xyz);

    float3 CamPos = GetCameraPosition();

    // Planet-relative, so the layer curves away to the horizon instead of running to infinity.
    float3 Origin = float3(0.0, kPlanetR + CamPos.y, 0.0);
    float  InnerR = kPlanetR + Pass().LayerBottom;
    float  OuterR = kPlanetR + max(Pass().LayerTop, Pass().LayerBottom + 1.0);

    float2 Outer = Sky_RaySphereIntersect(Origin, RayDir, OuterR);
    if (Outer.y <= 0.0)
    {
        return;
    }

    float2 Inner = Sky_RaySphereIntersect(Origin, RayDir, InnerR);

    float Start;
    float End;
    if (Inner.x > 0.0)
    {
        Start = max(Outer.x, 0.0);
        End   = Inner.x;
    }
    else if (Inner.y > 0.0)
    {
        Start = Inner.y;
        End   = Outer.y;
    }
    else
    {
        Start = max(Outer.x, 0.0);
        End   = Outer.y;
    }

    // Sampled by UV, not Load: P indexes the half-res target while depth is full-res.
    float2 Off = 0.5 / float2(Size);
    float  D0  = SampleTexture2DLevel(Pass().DepthIndex, SAMPLER_POINT_CLAMP, UV + float2(-Off.x, -Off.y), 0.0).r;
    float  D1  = SampleTexture2DLevel(Pass().DepthIndex, SAMPLER_POINT_CLAMP, UV + float2( Off.x, -Off.y), 0.0).r;
    float  D2  = SampleTexture2DLevel(Pass().DepthIndex, SAMPLER_POINT_CLAMP, UV + float2(-Off.x,  Off.y), 0.0).r;
    float  D3  = SampleTexture2DLevel(Pass().DepthIndex, SAMPLER_POINT_CLAMP, UV + float2( Off.x,  Off.y), 0.0).r;

    // Reverse-Z, so max is the CLOSEST surface in the footprint; occluding is the safer error at half res.
    float RawDepth = max(max(D0, D1), max(D2, D3));
    if (RawDepth > 0.0)
    {
        float3 SceneView  = ReconstructViewPos(UV, RawDepth, GetInverseCameraProjection());
        float3 SceneWorld = mul(GetInverseCameraView(), float4(SceneView, 1.0)).xyz;
        End = min(End, length(SceneWorld - CamPos));
    }

    End = min(End, Pass().MaxDistance);
    if (End <= Start)
    {
        return;
    }

    uint  Steps    = max(Pass().MarchSteps, 16u);
    float BaseStep = (End - Start) / float(Steps);

    float LayerThickness = max(OuterR - InnerR, 1.0);
    float CosTheta       = dot(RayDir, Pass().SunDirection);
    float Phase          = Cloud_Phase(CosTheta, Pass().ForwardScattering, Pass().BackScattering);

    float3 Scattered     = float3(0.0, 0.0, 0.0);
    float  Transmittance = 1.0;

    // Empty space costs one shape fetch, so stride through it and spend the budget inside cloud.
    const float kEmptyStepScale = 3.0;

    float T = Start + Cloud_IGN(float2(P)) * BaseStep;

    [loop]
    for (uint i = 0u; i < Steps && T < End; ++i)
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

        // Returns early on a zero shape without paying for the detail fetch.
        float Density = Cloud_SampleDensity(WorldSample, HeightFrac, false);
        if (Density <= 0.0)
        {
            T += BaseStep * kEmptyStepScale;
            continue;
        }

        T += BaseStep;

        // Widening light step: the far samples cover the cloud's bulk without paying full price.
        uint  LSteps   = max(Pass().LightSteps, 1u);
        float LStepLen = LayerThickness * 0.5 / float(LSteps);
        float SunDepth = 0.0;
        float LT       = 0.0;

        [loop]
        for (uint l = 0u; l < LSteps; ++l)
        {
            LT += LStepLen;
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

            SunDepth += Cloud_SampleDensity(LWorld, LHF, true) * LT;
        }

        float SunTransmittance = exp(-SunDepth);

        // Powder: thin sunward edges scatter out before they scatter back, so they read dark.
        float Powder = 1.0 - exp(-Density * BaseStep * 2.0);
        float Lit    = lerp(1.0, Powder, Pass().PowderStrength);

        float3 SunTerm     = Pass().SunColor * SunTransmittance * Phase * Lit;
        float3 AmbientTerm = Pass().AmbientColor * lerp(0.35, 1.0, HeightFrac);

        float StepTrans = exp(-Density * BaseStep);

        // Analytic segment integral rather than a Riemann sum, which keeps energy at coarse steps.
        float3 StepScatter = (SunTerm + AmbientTerm) * (1.0 - StepTrans);

        Scattered     += StepScatter * Transmittance;
        Transmittance *= StepTrans;
    }

    gRWTextures2D[Pass().ScatterUAV][P] = float4(SanitizeRadiance(Scattered), Transmittance);
}
