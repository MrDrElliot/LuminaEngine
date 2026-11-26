#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"
#include "Noise.slang"

// Pure evaluator with no global bindings, and SunDir points toward the sun so SunDir.y is elevation.

#define SKY_MODE_SOLID    0u
#define SKY_MODE_GRADIENT 1u
#define SKY_MODE_DYNAMIC  2u
#define SKY_MODE_HDRI     3u
// Not a mode. The SPEC_SKY_MODE default, meaning the shader branches on Misc.x at runtime.
#define SKY_MODE_RUNTIME  4u

struct FEnvironmentParams
{
    float4 SolidSkyColor;
    float4 ZenithColor;       // w=horizonExponent
    float4 HorizonColor;
    float4 GroundColor;
    float4 SunTint;           // w=sunIntensity
    float4 Misc;              // x=skyMode, y=sunDiscScale, z=skyExposure, w=mieG

    float4 NightSkyColor;     // w=nightBrightness
    float4 StarParams;        // x=density, y=brightness, z=twinkleSpeed, w=size
    float4 MoonParams;        // x=size, y=glowSize, z=brightness, w=autoPlaceFlag
    float4 MoonDirection;     // xyz=manual dir, w unused
    float4 GalaxyParams;      // x=intensity, y=tilt(rad)
    float4 HDRIParams;        // x=intensity, y=cos(yaw), z=sin(yaw)
    float4 AtmosphereParams;  // x=aerosol scale, y=moon phase, z=moonlit sky illuminance, w=altitude band the IBL was baked at
};

static const float kSunBaseAngle  = radians(0.53);
static const float kMoonBaseAngle = radians(0.50);
// The disc against its moonlit sky in the ratio the sun's disc keeps to the day sky, so it reads as a light and not a gray coin.
static const float kMoonDiscRadiance = 6.0;

static const float3 kRayleighCoeff       = float3(SKY_RAYLEIGH_R, SKY_RAYLEIGH_G, SKY_RAYLEIGH_B);
static const float  kMieCoeff            = SKY_MIE_SCATTERING;
static const float  kMieExtinctionRatio  = SKY_MIE_EXTINCTION_RATIO;
static const float3 kOzoneCoeff          = float3(SKY_OZONE_R, SKY_OZONE_G, SKY_OZONE_B);
static const float  kAtmosphereR         = SKY_ATMOSPHERE_RADIUS;
static const float  kPlanetR             = SKY_PLANET_RADIUS;
static const float  kRayleighH           = SKY_RAYLEIGH_SCALE_HEIGHT;
static const float  kMieH                = SKY_MIE_SCALE_HEIGHT;
static const float  kOzoneCenter         = SKY_OZONE_CENTER;
static const float  kOzoneWidth          = SKY_OZONE_HALF_WIDTH;

// Not SunIntensity, which is only the disc brightness and must not drive the whole sky.
static const float  kSolarIrradiance = SKY_SOLAR_ILLUMINANCE;

// Sun and skylight on level ground under a high sun, per unit of solar illuminance.
static const float  kNoonGroundIrradiance = 1.0;
// The ground below the horizon keeps the brightness the sky cube was tuned with, half of GroundColor under a noon sun.
static const float  kGroundRadianceScale = 0.5 / (kNoonGroundIrradiance * kSolarIrradiance);

// The planet's average reflectance, which lights the sky from below in the multiple scattering.
static const float  kPlanetAlbedo = 0.3;

static const float2 kTransmittanceLUTSize = float2(SKY_TRANSMITTANCE_LUT_WIDTH, SKY_TRANSMITTANCE_LUT_HEIGHT);
static const float  kMultiScatterLUTSize  = SKY_MULTISCATTER_LUT_SIZE;
static const float2 kSkyViewLUTSize       = float2(SKY_VIEW_LUT_WIDTH, SKY_VIEW_LUT_HEIGHT);

float Sky_RayleighPhase(float CosTheta)
{
    return (3.0 / (16.0 * PI)) * (1.0 + CosTheta * CosTheta);
}

// Henyey-Greenstein, which the clouds share.
float Sky_MiePhase(float CosTheta, float G)
{
    float G2    = G * G;
    float Denom = 1.0 + G2 - 2.0 * G * CosTheta;
    return (1.0 / (4.0 * PI)) * (1.0 - G2) / max(Denom * sqrt(max(Denom, 1e-6)), 1e-6);
}

// Cornette-Shanks, whose backscatter lobe keeps the sky opposite the sun from going flat the way Henyey-Greenstein leaves it.
float Sky_MiePhaseCS(float CosTheta, float G)
{
    float G2    = G * G;
    float K     = (3.0 / (8.0 * PI)) * (1.0 - G2) / (2.0 + G2);
    float Denom = 1.0 + G2 - 2.0 * G * CosTheta;
    return K * (1.0 + CosTheta * CosTheta) / max(Denom * sqrt(max(Denom, 1e-6)), 1e-6);
}

float2 Sky_RaySphereIntersect(float3 RayOrigin, float3 RayDir, float Radius)
{
    float B = dot(RayOrigin, RayDir);
    float C = dot(RayOrigin, RayOrigin) - Radius * Radius;
    float D = B * B - C;
    if (D < 0.0) return float2(-1.0, -1.0);
    float S = sqrt(D);
    return float2(-B - S, -B + S);
}

float Sky_OzoneDensity(float Height)
{
    return saturate(1.0 - abs(Height - kOzoneCenter) / kOzoneWidth);
}

// Amplitude starts at 0.5 and is left unnormalized, which is what the sky's tuning was authored against.
float Sky_FBM(float3 P) { return 0.5 * FBM3D(P, 5, 0.5, 2.02); }

// Quadratic spacing, since Mie's 1.2km scale height is nearly invisible to a uniform march over ~60km.
float Sky_StepFraction(int Index, int Count)
{
    float T = float(Index) / float(Count);
    return T * T;
}

// The camera's distance from the planet center, from its world height in meters.
float Sky_ViewRadius(float CameraY)
{
    return kPlanetR + SKY_GROUND_ALTITUDE + clamp(CameraY, 0.0, kAtmosphereR - kPlanetR - SKY_GROUND_ALTITUDE - 100.0);
}

struct FSkyMedium
{
    float3 ScatteringR;
    float3 ScatteringM;
    float3 Extinction;
};

// Aerosol scales the Mie particles alone, which is haze and humidity, so a humid evening glows warmer and wider.
FSkyMedium Sky_Medium(float Height, float Aerosol)
{
    FSkyMedium M;
    M.ScatteringR = kRayleighCoeff * exp(-Height / kRayleighH);
    M.ScatteringM = float3(kMieCoeff, kMieCoeff, kMieCoeff) * (exp(-Height / kMieH) * Aerosol);
    M.Extinction  = M.ScatteringR + M.ScatteringM * kMieExtinctionRatio + kOzoneCoeff * Sky_OzoneDensity(Height);
    return M;
}

// Texel centers sit on the ends of the unit range, so a lookup at either extreme reads that texel exactly.
float2 Sky_UnitToUV(float2 Unit, float2 Size) { return (Unit * (Size - 1.0) + 0.5) / Size; }

// Transmittance to the top of the atmosphere, in Bruneton's mapping, which spends its rows on the low heights where most air is.
float2 Sky_TransmittanceUV(float Radius, float CosZenith)
{
    // Factored squares, since a planet-sized radius squared leaves float no room for the height that matters.
    float H    = sqrt((kAtmosphereR - kPlanetR) * (kAtmosphereR + kPlanetR));
    float Rho  = sqrt(max((Radius - kPlanetR) * (Radius + kPlanetR), 0.0));
    float Disc = (kAtmosphereR - Radius) * (kAtmosphereR + Radius) + Radius * Radius * CosZenith * CosZenith;
    float D    = max(0.0, -Radius * CosZenith + sqrt(max(Disc, 0.0)));
    float DMin = kAtmosphereR - Radius;
    float DMax = Rho + H;
    return Sky_UnitToUV(float2((D - DMin) / max(DMax - DMin, 1e-3), Rho / H), kTransmittanceLUTSize);
}

void Sky_TransmittanceParams(float2 Unit, out float Radius, out float CosZenith)
{
    float H    = sqrt((kAtmosphereR - kPlanetR) * (kAtmosphereR + kPlanetR));
    float Rho  = H * Unit.y;
    Radius     = sqrt(Rho * Rho + kPlanetR * kPlanetR);
    float DMin = kAtmosphereR - Radius;
    float DMax = Rho + H;
    float D    = DMin + Unit.x * (DMax - DMin);
    CosZenith  = (D <= 0.0) ? 1.0 : clamp((H * H - Rho * Rho - D * D) / (2.0 * Radius * D), -1.0, 1.0);
}

float3 Sky_SampleTransmittance(FTexture2D Lut, float Radius, float CosZenith)
{
    return Lut.SampleLevel(Samplers::LinearClamp, Sky_TransmittanceUV(Radius, CosZenith), 0.0).rgb;
}

// Hillaire 2020 multiple scattering by light zenith and height in half 0, and the skylight it puts on level ground in half 1.
float3 Sky_SampleMultiScatterHalf(FTexture2D Lut, float Radius, float CosLightZenith, uint Half)
{
    float2 Unit = float2(CosLightZenith * 0.5 + 0.5, saturate((Radius - kPlanetR) / (kAtmosphereR - kPlanetR)));
    float2 UV   = Sky_UnitToUV(Unit, float2(kMultiScatterLUTSize, kMultiScatterLUTSize));
    return Lut.SampleLevel(Samplers::LinearClamp, float2((UV.x + float(Half)) * 0.5, UV.y), 0.0).rgb;
}

float3 Sky_SampleMultiScatter(FTexture2D Lut, float Radius, float CosLightZenith)
{
    return Sky_SampleMultiScatterHalf(Lut, Radius, CosLightZenith, 0u);
}

// The sky view, by the view's zenith angle and its azimuth from the light, with rows squeezed toward the horizon.
float2 Sky_SkyViewUnit(float Radius, float CosViewZenith, float CosLightView)
{
    float Vhorizon           = sqrt(max((Radius - kPlanetR) * (Radius + kPlanetR), 0.0));
    float Beta               = acos(clamp(Vhorizon / Radius, -1.0, 1.0));
    float ZenithHorizonAngle = PI - Beta;
    float ViewZenith         = acos(clamp(CosViewZenith, -1.0, 1.0));

    float V;
    if (ViewZenith < ZenithHorizonAngle)
    {
        float Coord = ViewZenith / ZenithHorizonAngle;
        V = (1.0 - sqrt(max(1.0 - Coord, 0.0))) * 0.5;
    }
    else
    {
        float Coord = (ViewZenith - ZenithHorizonAngle) / max(Beta, 1e-5);
        V = sqrt(saturate(Coord)) * 0.5 + 0.5;
    }

    float U = sqrt(saturate(-CosLightView * 0.5 + 0.5));
    return float2(U, V);
}

void Sky_SkyViewParams(float2 Unit, float Radius, out float CosViewZenith, out float CosLightView)
{
    float Vhorizon           = sqrt(max((Radius - kPlanetR) * (Radius + kPlanetR), 0.0));
    float Beta               = acos(clamp(Vhorizon / Radius, -1.0, 1.0));
    float ZenithHorizonAngle = PI - Beta;

    if (Unit.y < 0.5)
    {
        float Coord = 1.0 - 2.0 * Unit.y;
        CosViewZenith = cos(ZenithHorizonAngle * (1.0 - Coord * Coord));
    }
    else
    {
        float Coord = Unit.y * 2.0 - 1.0;
        CosViewZenith = cos(ZenithHorizonAngle + Beta * Coord * Coord);
    }

    float Coord = Unit.x * Unit.x;
    CosLightView = -(Coord * 2.0 - 1.0);
}

// Half 0 is the sun's sky and half 1 the moon's, each already scaled by its light.
float3 Sky_SampleSkyView(FTexture2D Lut, float Radius, float3 RayDir, float3 LightDir, uint Half)
{
    float2 RayFlat   = RayDir.xz;
    float2 LightFlat = LightDir.xz;
    float  RayLen    = dot(RayFlat, RayFlat);
    float  LightLen  = dot(LightFlat, LightFlat);
    // Straight up or a light at the zenith leaves no azimuth, and every azimuth reads the same there.
    float CosLightView = (RayLen > 1e-10 && LightLen > 1e-10) ? dot(RayFlat, LightFlat) * rsqrt(RayLen * LightLen) : 1.0;

    float2 Unit = Sky_SkyViewUnit(Radius, RayDir.y, CosLightView);
    float2 UV   = float2((float(Half) * kSkyViewLUTSize.x + Unit.x * (kSkyViewLUTSize.x - 1.0) + 0.5) / (2.0 * kSkyViewLUTSize.x),
                         (Unit.y * (kSkyViewLUTSize.y - 1.0) + 0.5) / kSkyViewLUTSize.y);
    return Lut.SampleLevel(Samplers::LinearClamp, UV, 0.0).rgb;
}

// The one integrator the transmittance-backed LUTs share, so the sky, its cube and the aerial haze cannot drift apart.
struct FSkyIntegral
{
    float3 Luminance;       // per unit of light illuminance
    float3 Transmittance;
    float3 MultiScatterAs1; // what one more bounce would scatter, the f_ms of the multiple scattering LUT
};

// The far end of a planet-centered ray inside the atmosphere, at the ground when it hits it.
float Sky_RayEnd(float3 Origin, float3 Dir, out bool bHitsGround)
{
    bHitsGround = false;
    float2 Atmo = Sky_RaySphereIntersect(Origin, Dir, kAtmosphereR);
    float  End  = max(Atmo.y, 0.0);
    float2 Ground = Sky_RaySphereIntersect(Origin, Dir, kPlanetR);
    if (Ground.x > 0.0 && Ground.x < End)
    {
        End = Ground.x;
        bHitsGround = true;
    }
    return End;
}

// Origin is planet-centered and inside the atmosphere. Without the multiple scattering LUT it integrates the single bounce that LUT is built from.
FSkyIntegral Sky_Integrate(FTexture2D TransmittanceLut, FTexture2D MultiScatterLut, bool bUseMultiScatter,
                           float3 Origin, float3 Dir, float3 LightDir, int Steps,
                           float Aerosol, float MieG, bool bIsotropic, float3 GroundAlbedo)
{
    FSkyIntegral Out;
    Out.Luminance       = float3(0.0, 0.0, 0.0);
    Out.Transmittance   = float3(1.0, 1.0, 1.0);
    Out.MultiScatterAs1 = float3(0.0, 0.0, 0.0);

    bool  bHitsGround;
    float TMax = Sky_RayEnd(Origin, Dir, bHitsGround);
    if (TMax <= 0.0)
    {
        return Out;
    }

    float CosTheta = dot(Dir, LightDir);
    float PhaseR   = bIsotropic ? 1.0 / (4.0 * PI) : Sky_RayleighPhase(CosTheta);
    float PhaseM   = bIsotropic ? 1.0 / (4.0 * PI) : Sky_MiePhaseCS(CosTheta, MieG);

    float PrevT = 0.0;
    [[loop]]
    for (int i = 0; i < Steps; ++i)
    {
        float NextT = TMax * Sky_StepFraction(i + 1, Steps);
        float Dt    = NextT - PrevT;
        float3 P    = Origin + Dir * (PrevT + Dt * 0.5);
        PrevT = NextT;

        float  Len    = length(P);
        float  Radius = max(Len, kPlanetR);
        float3 Up     = P / max(Len, 1.0);
        FSkyMedium M  = Sky_Medium(Radius - kPlanetR, Aerosol);
        float3 Scattering          = M.ScatteringR + M.ScatteringM;
        float3 SampleTransmittance = exp(-M.Extinction * Dt);

        float  CosLightZenith = dot(Up, LightDir);
        float3 ToLight        = Sky_SampleTransmittance(TransmittanceLut, Radius, CosLightZenith);

        // The ground hides the light once it is below this point's horizon, which is what darkens twilight from the bottom up.
        float PlanetShadow = (Sky_RaySphereIntersect(P + Up * 10.0, LightDir, kPlanetR).x > 0.0) ? 0.0 : 1.0;

        float3 PhaseScattering = M.ScatteringR * PhaseR + M.ScatteringM * PhaseM;
        float3 Multi = bUseMultiScatter ? Sky_SampleMultiScatter(MultiScatterLut, Radius, CosLightZenith) * Scattering : float3(0.0, 0.0, 0.0);

        // Integrated across the step analytically, so a long step through dense air does not overshoot.
        float3 S      = PlanetShadow * ToLight * PhaseScattering + Multi;
        float3 InvExt = 1.0 / max(M.Extinction, float3(1e-12, 1e-12, 1e-12));
        Out.Luminance       += Out.Transmittance * (S - S * SampleTransmittance) * InvExt;
        Out.MultiScatterAs1 += Out.Transmittance * (Scattering - Scattering * SampleTransmittance) * InvExt;
        Out.Transmittance   *= SampleTransmittance;
    }

    if (bHitsGround)
    {
        float3 Up  = normalize(Origin + Dir * TMax);
        float  CosLightZenith = dot(Up, LightDir);
        float3 ToLight  = Sky_SampleTransmittance(TransmittanceLut, kPlanetR, CosLightZenith);
        // The sky lights the ground too, which is all that holds it up once the sun is low.
        float3 Skylight = bUseMultiScatter ? Sky_SampleMultiScatterHalf(MultiScatterLut, kPlanetR, CosLightZenith, 1u) : float3(0.0, 0.0, 0.0);
        Out.Luminance += Out.Transmittance * (ToLight * saturate(CosLightZenith) + Skylight) * GroundAlbedo;
    }
    return Out;
}

// The moon trails the sun through the sky by its age in the month, so phase 0.5 sits exactly opposite the sun.
float3 Sky_MoonDirection(FEnvironmentParams Env, float3 SunDir)
{
    if (Env.MoonParams.w < 0.5)
    {
        return normalize(Env.MoonDirection.xyz);
    }

    float  Elongation = Env.AtmosphereParams.y * TWO_PI;
    float3 Up   = float3(0.0, 1.0, 0.0);
    float3 Perp = Up - SunDir * dot(Up, SunDir);
    float  Len  = length(Perp);
    Perp = (Len > 1e-4) ? Perp / Len : float3(1.0, 0.0, 0.0);
    return normalize(SunDir * cos(Elongation) + Perp * sin(Elongation));
}

// A disc whose edge is about one pixel wide however large it is drawn, so neither the sun nor the moon aliases or blurs.
float Sky_DiscMask(float Angle, float Radius, float PixelAngle)
{
    return saturate((Radius - Angle) / max(PixelAngle, Radius * 0.01) + 0.5);
}

// The chord form, since acos of a dot product near one loses most of the sun's quarter degree to rounding.
float Sky_AngleBetween(float3 A, float3 B)
{
    return 2.0 * asin(saturate(0.5 * length(A - B)));
}

// The disc darkens toward its rim, more in blue than in red, which is what makes a real sun read as a ball of gas.
float3 Sky_SunDisc(float3 RayDir, float3 SunDir, float DiscScale, float PixelAngle)
{
    float Radius = kSunBaseAngle * max(DiscScale, 0.0);
    if (Radius <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float Angle = Sky_AngleBetween(RayDir, SunDir);
    if (Angle > Radius * 6.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    float Mask = Sky_DiscMask(Angle, Radius, PixelAngle);

    float  R    = saturate(Angle / Radius);
    float  Mu   = sqrt(max(1.0 - R * R, 0.0));
    float3 Limb = 1.0 - float3(0.40, 0.50, 0.65) * (1.0 - Mu);

    // The aureole a sky view texel is too coarse to hold, forward scattering hugging the disc.
    float Aureole = 0.05 * exp(-max(Angle - Radius, 0.0) / Radius) * (1.0 - Mask);
    return Limb * Mask + Aureole;
}

float3 Sky_RenderSun(float3 RayDir, float3 SunDir, float3 SunColor, float SunDiscScale, float3 Transmittance, float PixelAngle)
{
    // Narrow enough that a sun sitting on the geometric horizon still renders at full strength.
    float Occlusion = smoothstep(-0.04, 0.0, SunDir.y);
    return SunColor * Transmittance * Sky_SunDisc(RayDir, SunDir, SunDiscScale, PixelAngle) * (0.8 * Occlusion);
}

// Drawn by day too, as the pale moon real skies show, while its halo only appears once the sky darkens.
float3 Sky_RenderMoon(float3 RayDir, float3 SunDir, float3 MoonDir, FEnvironmentParams Env, float3 Transmittance, float PixelAngle, float NightFactor)
{
    float Brightness = Env.MoonParams.z;
    if (Brightness <= 0.0) return float3(0.0);

    float Size      = max(Env.MoonParams.x, 0.0);
    float GlowSize  = max(Env.MoonParams.y, 0.0);
    float DiscAngle = kMoonBaseAngle * Size;
    float GlowAngle = DiscAngle * (1.0 + 8.0 * GlowSize);

    float Angle = Sky_AngleBetween(RayDir, MoonDir);
    if (Angle > max(GlowAngle, DiscAngle * 1.5) || DiscAngle <= 0.0) return float3(0.0);

    // Disc-local coordinates, fixed to the moon's face because it always turns the same side to the viewer.
    float3 Up      = abs(MoonDir.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 AxisU   = normalize(cross(MoonDir, Up));
    float3 AxisV   = cross(AxisU, MoonDir);
    float  SinDisc = max(sin(DiscAngle), 1e-5);
    float  U       = dot(RayDir, AxisU) / SinDisc;
    float  V       = dot(RayDir, AxisV) / SinDisc;
    float  W       = sqrt(saturate(1.0 - U * U - V * V));

    // The visible hemisphere faces back along the view ray, toward the camera.
    float3 N = normalize(-MoonDir * W + AxisU * U + AxisV * V);

    // Lommel-Seeliger, which leaves a full moon evenly bright to its rim as the real one is, instead of a shaded ball.
    float Mu0 = dot(N, SunDir);
    float Mu  = max(W, 1e-3);
    float Lit = Mu0 > 0.0 ? Mu0 / (Mu0 + Mu) * 2.0 : 0.0;

    // The opposition surge, the brightening near full moon as the shadows between grains hide behind the grains.
    float PhaseAngle = acos(clamp(dot(SunDir, -MoonDir), -1.0, 1.0));
    Lit *= 1.0 + 0.6 / (1.0 + tan(min(PhaseAngle, 3.0) * 0.5) / 0.06);

    // Dark basalt seas over bright highlands, coarse enough that the face reads at a glance.
    float2 Face    = float2(U, V);
    float  Seas    = smoothstep(0.52, 0.78, Sky_FBM(float3(Face * 1.6, 7.3)) + 0.25);
    float  Craters = 0.92 + 0.16 * ValueNoise3D(float3(Face * 9.0, 2.0));
    float3 Albedo  = lerp(float3(1.0, 0.97, 0.92), float3(0.62, 0.64, 0.68), Seas) * Craters;

    // Earth lights the dark side, most near new moon when Earth seen from the moon is nearly full.
    float LitFraction = (1.0 + cos(PhaseAngle)) * 0.5;
    float Earthshine  = 0.015 * (1.0 - LitFraction) * (1.0 - saturate(Lit));

    float DiscMask = Sky_DiscMask(Angle, DiscAngle, PixelAngle);
    float Halo     = 0.0;
    if (GlowSize > 0.0 && NightFactor > 0.0)
    {
        float Fall = saturate(1.0 - (Angle - DiscAngle) / max(GlowAngle - DiscAngle, 1e-5));
        // The halo follows how much lit face there is, so a thin crescent barely glows.
        Halo = 0.18 * Fall * Fall * Fall * (1.0 - DiscMask) * LitFraction * NightFactor;
    }

    float Horizon = smoothstep(-0.02, 0.01, MoonDir.y);
    return (Albedo * (Lit + Earthshine) * DiscMask * kMoonDiscRadiance + Halo) * Transmittance * (Brightness * Horizon);
}

float Sky_NightFactor(float3 SunDir)
{
    return smoothstep(0.05, -0.15, SunDir.y);
}

// Density is a hash threshold, so changing it does not reshuffle existing stars.
float3 Sky_Stars(float3 RayDir, FEnvironmentParams Env, float Time, float NightFactor, float3 Transmittance)
{
    if (NightFactor <= 0.0) return float3(0.0);
    float Density = saturate(Env.StarParams.x);
    if (Density <= 0.0) return float3(0.0);

    float Brightness = Env.StarParams.y;
    float Twinkle    = Env.StarParams.z;
    float SizeT      = saturate(Env.StarParams.w);

    // Scale chosen so a density near 0.5 fills the sky with several thousand stars.
    float3 P    = RayDir * 380.0;
    float3 Cell = floor(P);

    float H = Hash31(Cell);
    float Threshold = 1.0 - Density;
    if (H < Threshold) return float3(0.0);

    float Seed = (H - Threshold) / max(1e-4, 1.0 - Threshold);

    float3 StarPos = Cell + Hash33(Cell + 19.0);
    float  D       = length(P - StarPos);
    float  Core    = smoothstep(lerp(0.05, 0.45, SizeT), 0.0, D);

    // Few bright stars and many faint ones, the steep count by magnitude of the real sky.
    float Magnitude = pow(Seed, 6.0);

    // Scintillation grows with the air the light crosses, so stars low in the sky twinkle hard and overhead ones barely do.
    float Airmass = 1.0 / max(RayDir.y + 0.05, 0.05);
    float Amount  = saturate(0.08 * Airmass);
    float Tw      = 1.0 - Amount + Amount * (0.5 + 0.5 * sin(Time * Twinkle * (1.0 + Seed) + Seed * 31.4));

    // Hot blue-white through cool orange, weighted toward the white middle like a real star field.
    float  Temp = Hash31(Cell + 7.0);
    float3 Tint = Temp < 0.15 ? float3(0.72, 0.82, 1.0) : (Temp > 0.85 ? float3(1.0, 0.82, 0.62) : float3(1.0, 0.96, 0.9));

    return Tint * Core * Magnitude * Tw * Brightness * NightFactor * Transmittance;
}

// The mean the dropped fine octaves would have added, so the band keeps the brightness it was tuned at.
static const float kGalaxyDroppedOctaves = 0.5 * (0.0625 + 0.03125) * 0.5;

float3 Sky_Galaxy(float3 RayDir, FEnvironmentParams Env)
{
    float Intensity = Env.GalaxyParams.x;
    if (Intensity <= 0.0) return float3(0.0);

    float Tilt = Env.GalaxyParams.y;
    float CosT = cos(Tilt);
    float SinT = sin(Tilt);

    // Rotated so Y is the galactic pole, making |y| the angular distance from the band.
    float3 R = float3(RayDir.x * CosT + RayDir.y * SinT,
                     -RayDir.x * SinT + RayDir.y * CosT,
                      RayDir.z);

    float DistFromBand = abs(R.y);
    float BandShape    = smoothstep(0.45, 0.0, DistFromBand);
    if (BandShape <= 0.0) return float3(0.0);
    BandShape = BandShape * BandShape;

    // Three octaves for the clouds of stars and one for the grain, since this runs per pixel and finer octaves only cost.
    float N1 = 0.5 * FBM3D(RayDir * 7.0, 3, 0.5, 2.02) + kGalaxyDroppedOctaves;
    float N2 = ValueNoise3D(RayDir * 27.0 + 50.0);
    float Noise = saturate(N1 * 0.7 + N2 * 0.5 - 0.25);

    // Dust lanes split the bright core, where the galaxy's disc is thickest.
    float Lanes = 1.0 - 0.7 * smoothstep(0.55, 0.85, N2 + N1 * 0.4 + 0.2) * smoothstep(0.12, 0.0, DistFromBand);

    float  Hue      = ValueNoise3D(RayDir * 4.0 + 11.0);
    float3 BandTint = lerp(float3(0.55, 0.7, 1.0), float3(1.0, 0.85, 0.65), Hue);

    return BandTint * BandShape * Noise * Lanes * Intensity;
}

// The night sky's own glow comes from a layer 90km up, so it brightens toward the horizon where a sight line crosses the most of it.
float Sky_AirglowGain(float3 RayDir)
{
    float SinZenith2 = saturate(1.0 - RayDir.y * RayDir.y);
    float LayerRatio = kPlanetR / (kPlanetR + 90e3);
    float VanRhijn   = rsqrt(max(1.0 - LayerRatio * LayerRatio * SinZenith2, 0.05));
    // Half the full Van Rhijn swing, since the denser air low in the sky dims what the longer path gathers.
    return 0.5 + 0.5 * VanRhijn;
}

// Everything beyond the lit atmosphere at night, in the night sky color the user picked, dimmed by the air in front of it.
float3 Sky_NightLayers(float3 RayDir, FEnvironmentParams Env, float NightFactor, float3 Transmittance)
{
    float UpMask = smoothstep(-0.05, 0.1, RayDir.y);
    if (NightFactor <= 0.0 || UpMask <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    float3 Background = Env.NightSkyColor.rgb * (Env.NightSkyColor.w * Sky_AirglowGain(RayDir));
    return (Background + Sky_Galaxy(RayDir, Env)) * Transmittance * (NightFactor * UpMask);
}

float3 Sky_Solid(FEnvironmentParams Env)
{
    return Env.SolidSkyColor.rgb;
}

float3 Sky_Gradient(FEnvironmentParams Env, float3 SunDir, float3 RayDir, float3 SunTransmittance, float PixelAngle)
{
    float Y = RayDir.y;
    float HorizonExp = max(Env.ZenithColor.w, 0.05);

    float3 Color;
    if (Y >= 0.0)
    {
        float T = pow(saturate(Y), HorizonExp);
        Color   = lerp(Env.HorizonColor.rgb, Env.ZenithColor.rgb, T);
    }
    else
    {
        float T = smoothstep(0.0, -0.4, Y);
        Color   = lerp(Env.HorizonColor.rgb, Env.GroundColor.rgb, T);
    }

    float3 SunColor = Env.SunTint.rgb * Env.SunTint.w;
    Color += Sky_RenderSun(RayDir, SunDir, SunColor, Env.Misc.y, SunTransmittance, PixelAngle);
    return Color;
}

// The atmosphere lit by both the sun and the moon, plus what lies beyond it at night, in scene-linear exposed units.
float3 Sky_Dynamic(FEnvironmentParams Env, FTexture2D SkyView, FTexture2D TransmittanceLut, float Radius, float3 SunDir, float3 MoonDir, float3 RayDir)
{
    float3 Color = Sky_SampleSkyView(SkyView, Radius, RayDir, SunDir, 0u)
                 + Sky_SampleSkyView(SkyView, Radius, RayDir, MoonDir, 1u);

    float NightFactor = Sky_NightFactor(SunDir);
    if (NightFactor > 0.0)
    {
        float3 Transmittance = Sky_SampleTransmittance(TransmittanceLut, Radius, RayDir.y);
        Color += Sky_NightLayers(RayDir, Env, NightFactor, Transmittance);
    }

    // Scene-linear, since tonemapping here would also cap the sky cube that feeds IBL.
    return Color * max(Env.Misc.z, 1e-3);
}

// The visible sky's equirect and cube mappings.

// The visible sky and the IBL bake must share this mapping or reflections slide.
float2 Sky_EquirectUV(float3 RayDir, float CosYaw, float SinYaw)
{
    float3 R = float3(RayDir.x * CosYaw - RayDir.z * SinYaw,
                      RayDir.y,
                      RayDir.x * SinYaw + RayDir.z * CosYaw);

    float u = 0.5 + atan2(-R.z, R.x) * (1.0 / TWO_PI);
    float v = 0.5 - asin(clamp(R.y, -1.0, 1.0)) * INV_PI;
    return float2(u, v);
}

// Face order is +X, -X, +Y, -Y, +Z, -Z.
float3 CubeFaceDirection(uint Face, float2 UV)
{
    float u = UV.x * 2.0 - 1.0;
    float v = UV.y * 2.0 - 1.0;

    float3 Dir;
    if (Face == 0u)      Dir = float3( 1.0, -v,  -u  );
    else if (Face == 1u) Dir = float3(-1.0, -v,   u  );
    else if (Face == 2u) Dir = float3( u,    1.0, v  );
    else if (Face == 3u) Dir = float3( u,   -1.0,-v  );
    else if (Face == 4u) Dir = float3( u,   -v,   1.0);
    else                 Dir = float3(-u,   -v,  -1.0);

    return normalize(Dir);
}
