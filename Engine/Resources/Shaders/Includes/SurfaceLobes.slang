#pragma once
#include "SceneGlobals.slang"
#include "GBuffer.slang"
#include "IBLRuntime.slang"

// The lobes beyond the base Lit model, after the OpenPBR surface layering, evaluated only where a pixel asks for them.

// Wavelengths in micrometers the thin film interferes at, one per output channel.
static const float3 kThinFilmWavelengths = float3(0.65, 0.532, 0.45);

// Below this the Charlie distribution collapses into a spike the raster path cannot resolve.
static const float kMinFuzzAlpha = 0.07;

// How far into a subsurface object the sun can still shine through, per unit of Subsurface Radius.
static const float kSubsurfacePenetrationMeters = 0.12;

struct FExtendedLobes
{
    float3 T;
    float3 B;
    float  Anisotropy;
    float  HazeWeight;
    float  HazeRoughness;
    float3 Fuzz;
    float  FuzzAmount;
    float  FuzzAlpha;
    // The share of light the fuzz returns toward the eye, which the layers under it lose.
    float  FuzzAlbedoV;
    float  ThinFilmThickness;
    float  ThinFilmIOR;
    bool   bSubsurface;
    // Exponent on NdotL per channel, under 1 where light spreads further past the terminator inside the surface.
    float3 SubsurfacePower;
    float3 SubsurfaceColor;
    float  SubsurfaceRadius;
};

// Directional albedo of the Charlie sheen, a fit to its integral that is close at the roughness artists use.
float FuzzAlbedo(float NdotV, float Alpha)
{
    const float Grazing = 1.0 - saturate(NdotV);
    return saturate(lerp(0.08, 0.3, Alpha) + lerp(0.25, 0.45, Alpha) * Grazing * Grazing * Grazing);
}

FExtendedLobes MakeExtendedLobes(FGBufferData G, float3 N, float NdotV, bool bExtended, bool bSubsurface)
{
    FExtendedLobes X;
    const float3 Tangent = G.Tangent - N * dot(N, G.Tangent);
    X.T = dot(Tangent, Tangent) > 1e-8 ? normalize(Tangent) : float3(1.0, 0.0, 0.0);
    X.B = cross(N, X.T);

    X.Anisotropy        = bExtended ? clamp(G.Anisotropy, -0.95, 0.95) : 0.0;
    X.HazeWeight        = bExtended ? saturate(G.HazeWeight) : 0.0;
    X.HazeRoughness     = max(G.HazeRoughness, 0.045);
    X.Fuzz              = bExtended ? saturate(G.Fuzz) : float3(0.0, 0.0, 0.0);
    X.FuzzAmount        = max(X.Fuzz.r, max(X.Fuzz.g, X.Fuzz.b));
    X.FuzzAlpha         = max(G.FuzzRoughness * G.FuzzRoughness, kMinFuzzAlpha);
    X.FuzzAlbedoV       = X.FuzzAmount > 0.0 ? FuzzAlbedo(NdotV, X.FuzzAlpha) : 0.0;
    X.ThinFilmThickness = bExtended ? G.ThinFilmThickness : 0.0;
    X.ThinFilmIOR       = max(G.ThinFilmIOR, 1.0);

    X.bSubsurface      = bSubsurface;
    X.SubsurfaceColor  = saturate(G.Transmission);
    X.SubsurfaceRadius = saturate(G.Thickness);
    X.SubsurfacePower  = bSubsurface ? 1.0 / (1.0 + 2.5 * X.SubsurfaceRadius * X.SubsurfaceColor) : float3(1.0, 1.0, 1.0);
    return X;
}

// Interference of the light reflected off the film's top and its bottom (Airy summation), per wavelength.
float3 ThinFilmFresnel(float CosTheta1, float3 F0, float ThicknessMicrons, float FilmIOR)
{
    const float SinT2Sq = (1.0 - CosTheta1 * CosTheta1) / (FilmIOR * FilmIOR);
    if (SinT2Sq >= 1.0)
    {
        return float3(1.0, 1.0, 1.0);
    }
    const float CosTheta2 = sqrt(1.0 - SinT2Sq);

    // Air to film, which always reflects with the sign of a denser medium.
    const float  F0Top    = (FilmIOR - 1.0) * (FilmIOR - 1.0) / ((FilmIOR + 1.0) * (FilmIOR + 1.0));
    const float  RTop     = F0Top + (1.0 - F0Top) * pow(1.0 - CosTheta1, 5.0);
    const float  ATop     = -sqrt(RTop);

    // The base's index recovered from its F0, so a dense metal under the film keeps reflecting strongly.
    const float3 SqrtF0   = sqrt(clamp(F0, 0.0, 0.99));
    const float3 BaseIOR  = (1.0 + SqrtF0) / (1.0 - SqrtF0);
    const float3 F0Bottom = (BaseIOR - FilmIOR) * (BaseIOR - FilmIOR) / ((BaseIOR + FilmIOR) * (BaseIOR + FilmIOR));
    const float3 RBottom  = F0Bottom + (1.0 - F0Bottom) * pow(1.0 - CosTheta2, 5.0);
    const float3 ABottom  = select(BaseIOR > FilmIOR, -sqrt(RBottom), sqrt(RBottom));

    const float3 Phase = TWO_PI * (2.0 * FilmIOR * ThicknessMicrons * CosTheta2) / kThinFilmWavelengths;
    const float3 Cross = 2.0 * ATop * ABottom * cos(Phase);
    return saturate((ATop * ATop + ABottom * ABottom + Cross) / max(1.0 + ATop * ATop * ABottom * ABottom + Cross, 1e-4));
}

float3 ExtendedFresnel(FExtendedLobes X, float CosTheta, float3 F0)
{
    if (X.ThinFilmThickness > 1e-3)
    {
        return ThinFilmFresnel(CosTheta, F0, X.ThinFilmThickness, X.ThinFilmIOR);
    }
    return F0 + (1.0 - F0) * pow(1.0 - CosTheta, 5.0);
}

// Anisotropic GGX (Burley 2012) with the height-correlated Smith visibility (Heitz 2014), 1 / (4 NdotL NdotV) folded in.
float AnisotropicGGX(FExtendedLobes X, float Alpha, float3 H, float3 V, float3 L, float NdotH, float NdotV, float NdotL)
{
    const float At = max(Alpha * (1.0 + X.Anisotropy), 1e-3);
    const float Ab = max(Alpha * (1.0 - X.Anisotropy), 1e-3);

    const float ToH = dot(X.T, H);
    const float BoH = dot(X.B, H);
    const float Denom = ToH * ToH / (At * At) + BoH * BoH / (Ab * Ab) + NdotH * NdotH;
    const float D     = 1.0 / max(PI * At * Ab * Denom * Denom, 1e-8);

    const float ToV = dot(X.T, V);
    const float BoV = dot(X.B, V);
    const float ToL = dot(X.T, L);
    const float BoL = dot(X.B, L);
    const float LambdaV = NdotL * length(float3(At * ToV, Ab * BoV, NdotV));
    const float LambdaL = NdotV * length(float3(At * ToL, Ab * BoL, NdotL));
    const float Vis     = 0.5 / max(LambdaV + LambdaL, 1e-6);
    return D * Vis;
}

float IsotropicGGX(float Alpha, float NdotH, float NdotV, float NdotL)
{
    const float A2    = Alpha * Alpha;
    const float Denom = NdotH * NdotH * (A2 - 1.0) + 1.0;
    const float D     = A2 / max(PI * Denom * Denom, 1e-8);
    const float Vis   = 0.5 / max(NdotL * sqrt(NdotV * NdotV * (1.0 - A2) + A2) + NdotV * sqrt(NdotL * NdotL * (1.0 - A2) + A2), 1e-6);
    return D * Vis;
}

// Charlie sheen (Estevez and Kulla 2017) with Neubelt and Pettineo's visibility, the soft rim of velvet and dust.
float FuzzLobe(float Alpha, float NdotH, float NdotV, float NdotL)
{
    const float InvAlpha = 1.0 / Alpha;
    const float SinSq    = max(1.0 - NdotH * NdotH, 1e-5);
    const float D        = (2.0 + InvAlpha) * pow(SinSq, 0.5 * InvAlpha) / TWO_PI;
    const float Vis      = 1.0 / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 1e-4);
    return D * Vis;
}

// Diffuse and specular from one light through every extended lobe, the counterpart of EvaluateRadianceContributionSplit.
void EvaluateExtendedContributionSplit(FExtendedLobes X, float3 N, float3 V, float NdotV, float Roughness, float3 F0,
                                       float3 DiffuseBase, float3 EnergyComp, float3 L, float3 Radiance,
                                       out float3 OutDiffuse, out float3 OutSpecular)
{
    OutDiffuse  = float3(0.0, 0.0, 0.0);
    OutSpecular = float3(0.0, 0.0, 0.0);

    const float NdotL = dot(N, L);
    if (NdotL <= 0.0)
    {
        return;
    }

    const float3 H     = normalize(V + L);
    const float  NdotH = saturate(dot(N, H));
    const float  HdotV = saturate(dot(H, V));
    const float  Alpha = max(Roughness * Roughness, 1e-3);

    float Spec = AnisotropicGGX(X, Alpha, H, V, L, NdotH, NdotV, NdotL);
    if (X.HazeWeight > 0.0)
    {
        const float HazeAlpha = max(X.HazeRoughness * X.HazeRoughness, 1e-3);
        Spec = lerp(Spec, IsotropicGGX(HazeAlpha, NdotH, NdotV, NdotL), X.HazeWeight);
    }
    const float3 F = ExtendedFresnel(X, HdotV, F0);

    // Inside a subsurface object the diffuse falls off slower per channel, energy normalized to the Lambert it replaces.
    float3 Geometry = float3(NdotL, NdotL, NdotL);
    if (X.bSubsurface)
    {
        Geometry = pow(float3(NdotL, NdotL, NdotL), X.SubsurfacePower) * (X.SubsurfacePower + 2.0) / 3.0;
    }

    float3 Diffuse  = DiffuseBase * BurleyDiffuseScale(NdotL, NdotV, HdotV, Roughness) * Geometry;
    float3 Specular = F * EnergyComp * Spec * NdotL;

    if (X.FuzzAmount > 0.0)
    {
        // The fuzz sits on top, so what it scatters back is what the layers beneath it never receive.
        const float BaseScale = 1.0 - X.FuzzAmount * max(X.FuzzAlbedoV, FuzzAlbedo(NdotL, X.FuzzAlpha));
        Diffuse  *= BaseScale;
        Specular  = Specular * BaseScale + X.Fuzz * FuzzLobe(X.FuzzAlpha, NdotH, NdotV, NdotL) * NdotL;
    }

    OutDiffuse  = Diffuse * Radiance;
    OutSpecular = Specular * Radiance;
}

// Light the sun pushes through a subsurface object, fading with the depth of surface it crossed on the way.
float3 EvaluateSubsurfaceTransmission(FExtendedLobes X, float3 N, float3 V, float3 L, float3 Radiance, float OccluderDepth)
{
    const float  Reach     = max(kSubsurfacePenetrationMeters * X.SubsurfaceRadius, 1e-3);
    const float3 Through   = exp(-OccluderDepth / (Reach * max(X.SubsurfaceColor, 0.05)));
    const float3 BackLight = normalize(L + N * 0.3);
    const float  Forward   = pow(saturate(dot(V, -BackLight)), 3.0);
    return X.SubsurfaceColor * Through * Forward * Radiance;
}

// Filament's bent reflection for anisotropy, which leans the lookup toward the direction the highlight stretches.
float3 AnisotropicReflection(FExtendedLobes X, float3 N, float3 V, float Roughness)
{
    if (abs(X.Anisotropy) < 1e-3)
    {
        return reflect(-V, N);
    }
    const float3 Direction = X.Anisotropy >= 0.0 ? X.B : X.T;
    const float3 AnisoT    = cross(Direction, V);
    const float3 AnisoN    = cross(AnisoT, Direction);
    const float3 Bent      = normalize(lerp(N, AnisoN, abs(X.Anisotropy) * saturate(5.0 * Roughness)));
    return reflect(-V, Bent);
}

// The image-based counterpart of EvaluateExtendedContributionSplit, through the same split-sum terms as EvaluateIBLAmbientSplit.
void EvaluateIBLAmbientExtendedSplit(FExtendedLobes X, float3 WorldPos, float3 N, float3 BentN, float3 V, float NdotV, float2 DFG,
                                     float3 Albedo, float3 F0, float Roughness, float Metallic, float AO,
                                     out float3 OutDiffuse, out float3 OutSpecular)
{
    const float3 R = AnisotropicReflection(X, N, V, Roughness);

    // The film's own reflectance replaces the Schlick curve, already carrying its rise toward grazing.
    float3 FssEss;
    if (X.ThinFilmThickness > 1e-3)
    {
        FssEss = ThinFilmFresnel(NdotV, F0, X.ThinFilmThickness, X.ThinFilmIOR) * (DFG.x + DFG.y);
    }
    else
    {
        FssEss = FresnelSchlickRoughness(NdotV, F0, Roughness) * DFG.x + DFG.y;
    }

    float  Ems    = saturate(1.0 - (DFG.x + DFG.y));
    float3 Favg   = F0 + (1.0 - F0) / 21.0;
    float3 FmsEms = (Ems * FssEss * Favg) / max(1.0 - Favg * Ems, 1e-4);

    float3 DiffuseColor = Albedo * (1.0 - Metallic);
    float3 kD           = DiffuseColor * (1.0 - FssEss - FmsEms);

    float3 Irradiance = SampleDiffuseIrradiance(WorldPos, BentN);
    float3 PreFilt    = SampleSpecularRadiance(WorldPos, R, Roughness);
    float  SpecAO     = ComputeBentSpecularOcclusion(BentN, N, R, NdotV, AO, Roughness);

    float3 Specular = FssEss * PreFilt * SpecAO;
    if (X.HazeWeight > 0.0)
    {
        const float2 HazeDFG   = SampleBRDFLUT(NdotV, X.HazeRoughness);
        const float3 HazeFss   = FresnelSchlickRoughness(NdotV, F0, X.HazeRoughness) * HazeDFG.x + HazeDFG.y;
        const float3 HazeRad   = SampleSpecularRadiance(WorldPos, R, X.HazeRoughness);
        const float  HazeAO    = ComputeBentSpecularOcclusion(BentN, N, R, NdotV, AO, X.HazeRoughness);
        Specular = lerp(Specular, HazeFss * HazeRad * HazeAO, X.HazeWeight);
    }

    float3 Diffuse = (FmsEms * AO + kD * AOMultiBounce(AO, DiffuseColor)) * Irradiance;

    if (X.FuzzAmount > 0.0)
    {
        const float  BaseScale = 1.0 - X.FuzzAmount * X.FuzzAlbedoV;
        const float3 FuzzRad   = SampleSpecularRadiance(WorldPos, reflect(-V, N), sqrt(X.FuzzAlpha));
        Diffuse  *= BaseScale;
        Specular  = Specular * BaseScale + X.Fuzz * X.FuzzAlbedoV * FuzzRad * AO;
    }

    OutDiffuse  = Diffuse;
    OutSpecular = Specular;
}
