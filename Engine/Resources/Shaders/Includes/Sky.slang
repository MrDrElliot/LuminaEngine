#pragma once

// Pure sky evaluator shared by the fullscreen sky and the IBL cubemap capture; no global bindings.
// Ray is world-space, +Y up; SunDir points from surface to sun, so SunDir.y is elevation in [-1, 1].

#define SKY_MODE_SOLID    0u
#define SKY_MODE_GRADIENT 1u
#define SKY_MODE_DYNAMIC  2u
#define SKY_MODE_HDRI     3u

struct FEnvironmentParams
{
    float4 SolidSkyColor;     // rgb=color
    float4 ZenithColor;       // rgb=color, w=horizonExponent
    float4 HorizonColor;      // rgb=color
    float4 GroundColor;       // rgb=color
    float4 SunTint;           // rgb=tint, w=sunIntensity
    float4 Misc;              // x=skyMode, y=sunDiscScale, z=skyExposure, w=mieG

    float4 NightSkyColor;     // rgb=tint, w=nightBrightness
    float4 StarParams;        // x=density, y=brightness, z=twinkleSpeed, w=size
    float4 MoonParams;        // x=size, y=glowSize, z=brightness, w=autoOpposeFlag
    float4 MoonDirection;     // xyz=manual dir, w unused
    float4 GalaxyParams;      // x=intensity, y=tilt(rad)
};

static const float kPI         = 3.14159265358979;
static const float kSunBaseAngle  = radians(0.53);
static const float kMoonBaseAngle = radians(0.50);

// Earth-like atmospheric coefficients (m^-1). Rayleigh + Mie + ozone, no LUT-based atmosphere.
static const float3 kRayleighCoeff = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float  kMieCoeff      = 2.1e-5;
static const float3 kOzoneCoeff    = float3(0.65e-6, 1.881e-6, 0.085e-6);
static const float  kAtmosphereR   = 6420e3;
static const float  kPlanetR       = 6360e3;
static const float  kRayleighH     = 8e3;
static const float  kMieH          = 1.2e3;
static const float  kOzoneCenter   = 25e3;
static const float  kOzoneWidth    = 15e3;

float Sky_RayleighPhase(float CosTheta)
{
    return (3.0 / (16.0 * kPI)) * (1.0 + CosTheta * CosTheta);
}

float Sky_MiePhase(float CosTheta, float G)
{
    float G2    = G * G;
    float Denom = 1.0 + G2 - 2.0 * G * CosTheta;
    return (1.0 / (4.0 * kPI)) * (1.0 - G2) / max(Denom * sqrt(max(Denom, 1e-6)), 1e-6);
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

// 3D hash -> [0,1].
float Sky_Hash31(float3 P)
{
    P = frac(P * 0.3183099 + 0.1);
    P *= 17.0;
    return frac(P.x * P.y * P.z * (P.x + P.y + P.z));
}

// 3D hash -> 3D point in [0,1].
float3 Sky_Hash33(float3 P)
{
    return float3(Sky_Hash31(P + 17.0),
                  Sky_Hash31(P + 53.0),
                  Sky_Hash31(P + 91.0));
}

// Smooth value noise on a sphere direction.
float Sky_ValueNoise(float3 P)
{
    float3 I = floor(P);
    float3 F = P - I;
    F = F * F * (3.0 - 2.0 * F);

    float n000 = Sky_Hash31(I + float3(0,0,0));
    float n100 = Sky_Hash31(I + float3(1,0,0));
    float n010 = Sky_Hash31(I + float3(0,1,0));
    float n110 = Sky_Hash31(I + float3(1,1,0));
    float n001 = Sky_Hash31(I + float3(0,0,1));
    float n101 = Sky_Hash31(I + float3(1,0,1));
    float n011 = Sky_Hash31(I + float3(0,1,1));
    float n111 = Sky_Hash31(I + float3(1,1,1));

    float nx00 = lerp(n000, n100, F.x);
    float nx10 = lerp(n010, n110, F.x);
    float nx01 = lerp(n001, n101, F.x);
    float nx11 = lerp(n011, n111, F.x);
    float nxy0 = lerp(nx00, nx10, F.y);
    float nxy1 = lerp(nx01, nx11, F.y);
    return lerp(nxy0, nxy1, F.z);
}

float Sky_FBM(float3 P)
{
    float V = 0.0;
    float A = 0.5;
    [[unroll]]
    for (int i = 0; i < 5; i++)
    {
        V += A * Sky_ValueNoise(P);
        P *= 2.02;
        A *= 0.5;
    }
    return V;
}

// Atmospheric scattering (Bruneton-style single-scatter + ozone).
float3 Sky_OpticalDepth(float3 RayOrigin, float3 RayDir, float RayLength)
{
    // 4 inner steps: the light-march integral converges fast as the sun ray climbs out
    // monotonically. The cube bake's per-fragment cost is dominated by these samples.
    const int Steps    = 4;
    float StepSize     = RayLength / float(Steps);
    float3 OpticalDepth = float3(0.0, 0.0, 0.0);

    // [[loop]]: unrolling holds every step's heavy temporaries live at once (register spike on the SkyCube
    // capture pass); rolling keeps one step. The serial exp() chain is latency-bound regardless.
    [[loop]]
    for (int i = 0; i < Steps; i++)
    {
        float3 P = RayOrigin + RayDir * (float(i) + 0.5) * StepSize;
        float  H = length(P) - kPlanetR;
        if (H < 0.0) break;

        OpticalDepth += float3(exp(-H / kRayleighH),
                               exp(-H / kMieH),
                               Sky_OzoneDensity(H)) * StepSize;
    }
    return OpticalDepth;
}

float3 Sky_Atmosphere(float3 RayOrigin, float3 RayDir, float3 SunDir, float3 SunColor, float MieG)
{
    float2 Atmo = Sky_RaySphereIntersect(RayOrigin, RayDir, kAtmosphereR);
    if (Atmo.y < 0.0) return float3(0.0, 0.0, 0.0);

    float Start = max(Atmo.x, 0.0);
    float End   = Atmo.y;

    // Trim against the planet so rays through the atmosphere don't pick up
    // light from the far side when looking at the ground.
    float2 Ground = Sky_RaySphereIntersect(RayOrigin, RayDir, kPlanetR);
    if (Ground.x > 0.0) End = min(End, Ground.x);

    float Length = End - Start;
    if (Length <= 0.0) return float3(0.0, 0.0, 0.0);

    // 8 outer steps x 4-step inner march = 32 nested evals (was 128). The sky is
    // low-frequency so the visual difference is imperceptible.
    const int Steps = 8;
    float StepSize  = Length / float(Steps);

    float3 SumR = float3(0.0, 0.0, 0.0);
    float3 SumM = float3(0.0, 0.0, 0.0);
    float3 PrimaryOD = float3(0.0, 0.0, 0.0);

    [[loop]]
    for (int i = 0; i < Steps; i++)
    {
        float3 P = RayOrigin + RayDir * (Start + (float(i) + 0.5) * StepSize);
        float  H = length(P) - kPlanetR;
        if (H < 0.0) break;

        float DensR = exp(-H / kRayleighH);
        float DensM = exp(-H / kMieH);
        float DensO = Sky_OzoneDensity(H);

        PrimaryOD += float3(DensR, DensM, DensO) * StepSize;

        // Planet-shadow test: skip columns where the planet occludes the sun. Without it
        // the night sky stays bright (the light optical-depth integral truncates at the ground).
        float2 SunGroundHit = Sky_RaySphereIntersect(P, SunDir, kPlanetR);
        if (SunGroundHit.x > 0.0) continue;

        float2 LightHit  = Sky_RaySphereIntersect(P, SunDir, kAtmosphereR);
        float3 LightOD   = Sky_OpticalDepth(P, SunDir, LightHit.y);
        float3 TotalOD   = PrimaryOD + LightOD;

        float3 TauR = kRayleighCoeff * TotalOD.x;
        float3 TauM = float3(kMieCoeff, kMieCoeff, kMieCoeff) * TotalOD.y * 1.1;
        float3 TauO = kOzoneCoeff * TotalOD.z;
        float3 Transmittance = exp(-(TauR + TauM + TauO));

        SumR += DensR * Transmittance * StepSize;
        SumM += DensM * Transmittance * StepSize;
    }

    float CosTheta = dot(RayDir, SunDir);
    float3 Rayleigh = kRayleighCoeff * Sky_RayleighPhase(CosTheta) * SumR;
    float3 Mie      = float3(kMieCoeff, kMieCoeff, kMieCoeff) * Sky_MiePhase(CosTheta, MieG) * SumM;

    const float kMsStrength = 0.9;
    const float kMsTint     = 0.15;
    float3 MsEnergy     = kRayleighCoeff * SumR;
    float  MsGrey       = dot(MsEnergy, float3(0.2126, 0.7152, 0.0722));
    float3 MultiScatter = lerp(float3(MsGrey), MsEnergy, kMsTint) * kMsStrength / kPI;

    return SunColor * (Rayleigh + Mie + MultiScatter);
}

float3 Sky_SunTimeOfDayColor(float SunHeight)
{
    float3 NightColor    = float3(0.10, 0.15, 0.30) * 0.04;
    float3 TwilightColor = float3(1.00, 0.40, 0.15) * 0.60;
    float3 SunriseColor  = float3(1.00, 0.70, 0.30) * 0.90;
    float3 DayColor      = float3(1.00, 0.95, 0.85) * 1.00;

    if (SunHeight < -0.3)  return NightColor;
    if (SunHeight < -0.05) return lerp(NightColor,    TwilightColor, smoothstep(-0.3,  -0.05, SunHeight));
    if (SunHeight <  0.05) return lerp(TwilightColor, SunriseColor,  smoothstep(-0.05,  0.05, SunHeight));
    if (SunHeight <  0.40) return lerp(SunriseColor,  DayColor,      smoothstep( 0.05,  0.40, SunHeight));
    return DayColor;
}

float3 Sky_RenderSun(float3 RayDir, float3 SunDir, float3 SunColor, float SunDiscScale)
{
    float SunDot     = dot(RayDir, SunDir);
    float DiscAngle  = kSunBaseAngle * max(SunDiscScale, 0.0);

    float CosDiscOuter = cos(DiscAngle * 1.4);
    float CosDiscInner = cos(DiscAngle * 0.5);
    float CosGlowOuter = cos(DiscAngle * 1.8);
    float CosGlowInner = cos(DiscAngle * 1.1);

    float SunDisc  = smoothstep(CosDiscOuter, CosDiscInner, SunDot);
    float GlowBase = smoothstep(CosGlowOuter, CosGlowInner, SunDot);
    float SunGlow  = GlowBase * GlowBase * GlowBase;

    float SunFade = smoothstep(0.0, 0.15, SunDir.y);
    return SunColor * (SunDisc * 0.8 + SunGlow * 0.3) * SunFade;
}

// One star per cell, sampling only the ray's cell (sub-pixel boundary gaps). Density is a
// hash threshold, so changing it doesn't reshuffle existing stars.
float3 Sky_Stars(float3 RayDir, FEnvironmentParams Env, float Time, float NightFactor)
{
    if (NightFactor <= 0.0) return float3(0.0);
    float Density   = saturate(Env.StarParams.x);
    if (Density <= 0.0) return float3(0.0);

    float Brightness = Env.StarParams.y;
    float Twinkle    = Env.StarParams.z;
    float SizeT      = saturate(Env.StarParams.w);

    // Quantize direction to a high-frequency grid. The scale chosen so a
    // moderate density (~0.5) fills the sky with several thousand stars.
    float3 P    = RayDir * 380.0;
    float3 Cell = floor(P);
    float3 F    = P - Cell;

    float H = Sky_Hash31(Cell);
    float Threshold = 1.0 - Density;
    if (H < Threshold) return float3(0.0);

    float Seed = (H - Threshold) / max(1e-4, 1.0 - Threshold);

    float3 Offset    = Sky_Hash33(Cell + 19.0);
    float3 StarPos   = Cell + Offset;
    float  D         = length(P - StarPos);

    float Radius = lerp(0.05, 0.45, SizeT);
    float Core   = smoothstep(Radius, 0.0, D);

    // Magnitude curve: most stars dim, rare ones bright.
    float Magnitude = pow(Seed, 6.0);

    // Twinkle: per-star phase + speed.
    float Tw = 0.6 + 0.4 * sin(Time * Twinkle + Seed * 12.566);

    // Color: cool / warm interpolation by seed.
    float3 Tint = lerp(float3(0.7, 0.85, 1.0), float3(1.0, 0.92, 0.78), Seed);

    // Fade stars within the sun's glare cone so the sun area stays clean
    // even when only barely below the horizon.
    return Tint * Core * Magnitude * Tw * Brightness * NightFactor;
}

// Procedural Milky-Way band: FBM noise restricted to a tilted galactic plane (tilt in radians).
float3 Sky_Galaxy(float3 RayDir, FEnvironmentParams Env, float NightFactor)
{
    float Intensity = Env.GalaxyParams.x;
    if (NightFactor <= 0.0 || Intensity <= 0.0) return float3(0.0);

    float Tilt = Env.GalaxyParams.y;
    float CosT = cos(Tilt);
    float SinT = sin(Tilt);

    // Rotate ray into a frame whose Y axis is the galactic pole, then the
    // |y| of the rotated vector is the angular distance from the band.
    float3 R = float3(RayDir.x * CosT + RayDir.y * SinT,
                     -RayDir.x * SinT + RayDir.y * CosT,
                      RayDir.z);

    float DistFromBand = abs(R.y);
    float BandShape    = smoothstep(0.45, 0.0, DistFromBand);
    BandShape = BandShape * BandShape;

    // Two-octave noise gives clumps of brightness along the band.
    float N1 = Sky_FBM(RayDir * 7.0);
    float N2 = Sky_FBM(RayDir * 18.0 + 50.0);
    float Noise = saturate(N1 * 0.7 + N2 * 0.5 - 0.25);

    // A faint cool/warm tint based on a third noise field gives variation
    // between blue-white and dust-warm regions of the band.
    float Hue = Sky_ValueNoise(RayDir * 4.0 + 11.0);
    float3 BandTint = lerp(float3(0.55, 0.7, 1.0), float3(1.0, 0.85, 0.65), Hue);

    return BandTint * BandShape * Noise * Intensity * NightFactor;
}

float3 Sky_RenderMoon(float3 RayDir, float3 SunDir, FEnvironmentParams Env, float NightFactor)
{
    float Brightness = Env.MoonParams.z;
    if (NightFactor <= 0.0 || Brightness <= 0.0) return float3(0.0);

    bool   bAuto    = Env.MoonParams.w >= 0.5;
    float3 MoonDir  = bAuto ? -SunDir : normalize(Env.MoonDirection.xyz);
    float  Size     = max(Env.MoonParams.x, 0.0);
    float  GlowSize = max(Env.MoonParams.y, 0.0);

    float MoonDot   = dot(RayDir, MoonDir);
    float DiscAngle = kMoonBaseAngle * Size;
    float CosDisc   = cos(DiscAngle);
    float CosGlow   = cos(DiscAngle * (1.0 + 8.0 * GlowSize));

    if (MoonDot < CosGlow) return float3(0.0);

    // Reconstruct the disc surface normal so phase shading reads as a sphere: build a local
    // frame on MoonDir, map RayDir into it, recover the third axis from the unit-sphere constraint.
    float3 Up      = abs(MoonDir.y) < 0.95 ? float3(0,1,0) : float3(1,0,0);
    float3 AxisU   = normalize(cross(MoonDir, Up));
    float3 AxisV   = cross(AxisU, MoonDir);
    float  SinDisc = max(sin(DiscAngle), 1e-4);
    float  U       = dot(RayDir, AxisU) / SinDisc;
    float  V       = dot(RayDir, AxisV) / SinDisc;
    float  W2      = saturate(1.0 - U * U - V * V);
    float  W       = sqrt(W2);

    float3 N = normalize(MoonDir * W + AxisU * U + AxisV * V);

    // Lit by the sun. Lambert with a soft floor so a "new moon" still hints
    // at outline rather than vanishing.
    float Lambert = saturate(dot(N, SunDir));
    float Phase   = lerp(0.04, 1.0, pow(Lambert, 0.85));

    // Subtle surface variation: a coarse FBM gives the impression of mare
    // patches without trying to match the real moon.
    float Surface = 0.85 + 0.3 * Sky_FBM(N * 4.0);
    Phase *= Surface;

    float3 MoonAlbedo = float3(0.95, 0.93, 0.88);
    float DiscMask    = smoothstep(CosDisc - 1e-4, CosDisc, MoonDot);
    float GlowMask    = smoothstep(CosGlow, CosDisc, MoonDot);
    float Glow        = GlowMask * GlowMask * 0.18;

    return MoonAlbedo * (DiscMask * Phase + Glow) * Brightness * NightFactor;
}

float3 Sky_Solid(FEnvironmentParams Env)
{
    return Env.SolidSkyColor.rgb;
}

float3 Sky_Gradient(FEnvironmentParams Env, float3 SunDir, float3 RayDir)
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
    Color += Sky_RenderSun(RayDir, SunDir, SunColor, Env.Misc.y);
    return Color;
}

float3 Sky_Dynamic(FEnvironmentParams Env, float3 SunDir, float3 RayDir, float Time)
{
    float SunHeight = SunDir.y;
    float MieG      = clamp(Env.Misc.w, -0.99, 0.99);
    float3 SunColor = Sky_SunTimeOfDayColor(SunHeight) * Env.SunTint.rgb * Env.SunTint.w;

    // Atmosphere math is in planet-relative coords. Origin at 50m altitude
    // so the camera never falls inside the planet sphere.
    float3 AtmoCamera = float3(0.0, kPlanetR + 50.0, 0.0);
    float3 Atmo = Sky_Atmosphere(AtmoCamera, RayDir, SunDir, SunColor, MieG);

    // Night blend: 1 (deep night) at SunHeight ~ -0.15 to 0 (full day) at ~ 0, via sun elevation.
    float NightFactor = smoothstep(0.05, -0.15, SunHeight);

    // Below-horizon ground tint: the atmosphere term goes black there, so blend in a
    // darkened ground swatch to avoid a hard cutoff line.
    if (RayDir.y < 0.0)
    {
        float Below = smoothstep(0.0, -0.25, RayDir.y);
        float3 GroundTint = Env.GroundColor.rgb * 0.5;
        Atmo = lerp(Atmo, GroundTint, Below);
    }

    // Galaxy lives in the cube (low-frequency, fine at 256). Stars/sun/moon are pixel-scale and
    // alias at cube resolution, so Environment.slang re-evaluates them per-fragment instead.
    float UpMask = smoothstep(-0.05, 0.1, RayDir.y);
    float3 Galaxy = float3(0.0);
    if (UpMask > 0.0 && NightFactor > 0.0)
    {
        Galaxy = Sky_Galaxy(RayDir, Env, NightFactor) * UpMask;
    }

    // Background night color sits underneath stars/galaxy and shows through
    // the dim atmosphere when the sun is below the horizon.
    float3 NightBG = Env.NightSkyColor.rgb * Env.NightSkyColor.w * NightFactor * UpMask;

    float3 Color = Atmo + NightBG + Galaxy;

    // Tonemap with the artist's exposure; Reinhard-ish roll-off avoids crushing moon/stars to white.
    float Exposure = max(Env.Misc.z, 1e-3);
    Color *= Exposure;
    Color = Color / (1.0 + Color);

    // Slight contrast lift in the midtones.
    Color = lerp(Color, Color * Color * (3.0 - 2.0 * Color), 0.1);
    return Color;
}

float3 EvaluateSky(FEnvironmentParams Env, float3 SunDir, float3 RayDir, float Time)
{
    uint Mode = asuint(Env.Misc.x);

    if (Mode == SKY_MODE_SOLID)    return Sky_Solid(Env);
    if (Mode == SKY_MODE_GRADIENT) return Sky_Gradient(Env, SunDir, RayDir);
    return Sky_Dynamic(Env, SunDir, RayDir, Time);
}

// Cubemap face -> world-space ray direction.
//   Face 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z
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
