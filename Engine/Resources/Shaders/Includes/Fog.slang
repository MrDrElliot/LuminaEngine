#pragma once
#include "SceneGlobals.slang"
#include "Froxel.slang"

// Normalized density at a height, clamped to 1 at and below BaseH so the profile cannot run away.
float Fog_HeightTerm(float Falloff, float BaseH, float Y)
{
    return exp(clamp(-Falloff * (Y - BaseH), -60.0, 0.0));
}

// Density is uniform at the base value below FogBaseHeight and falls off exponentially above it.
float Fog_DensityAt(FExponentialHeightFogParams Fog, float3 WorldPos)
{
    return Fog.InscatteringColor.w * Fog_HeightTerm(Fog.HeightParams.x, Fog.HeightParams.y, WorldPos.y);
}

// A difference of endpoint height terms, so neither exponent's underflow clamp can survive alone.
float Fog_ExpSegmentOpticalDepth(float D0, float Falloff, float BaseH, float YStart, float YEnd, float FVy)
{
    return D0 * (Fog_HeightTerm(Falloff, BaseH, YStart) - Fog_HeightTerm(Falloff, BaseH, YEnd)) / FVy;
}

// Optical depth of Fog_DensityAt along a ray segment, split at the base-height crossing.
float Fog_OpticalDepth(FExponentialHeightFogParams Fog, float3 StartPos, float3 RayDir, float SegLen)
{
    float Falloff = Fog.HeightParams.x;
    float BaseH   = Fog.HeightParams.y;
    float D0      = Fog.InscatteringColor.w;
    float Y0      = StartPos.y;
    float Vy      = RayDir.y;

    // Near-horizontal ray or no falloff, so the height term is constant over the segment.
    if (abs(Falloff * Vy) <= 1e-5)
    {
        return Fog_DensityAt(Fog, StartPos) * SegLen;
    }

    float TSplit     = clamp((BaseH - Y0) / Vy, 0.0, SegLen);
    float AboveStart = Vy > 0.0 ? TSplit : 0.0;
    float AboveLen   = Vy > 0.0 ? SegLen - TSplit : TSplit;

    float OD = D0 * (SegLen - AboveLen);
    if (AboveLen > 0.0)
    {
        float Ya = max(Y0 + Vy * AboveStart, BaseH);
        float Yb = max(Ya + Vy * AboveLen, BaseH);
        OD += Fog_ExpSegmentOpticalDepth(D0, Falloff, BaseH, Ya, Yb, Falloff * Vy);
    }
    return OD;
}

// Optical depth from StartPos to infinity, for sky pixels. Only climbing rays leave the medium.
float Fog_OpticalDepthToInfinity(FExponentialHeightFogParams Fog, float3 StartPos, float3 RayDir)
{
    float Falloff = Fog.HeightParams.x;
    float BaseH   = Fog.HeightParams.y;
    float D0      = Fog.InscatteringColor.w;
    float Vy      = RayDir.y;
    float FVy     = Falloff * Vy;

    if (FVy > 1e-5)
    {
        float BelowLen = max((BaseH - StartPos.y) / Vy, 0.0);
        float Ya       = max(StartPos.y + Vy * BelowLen, BaseH);
        return D0 * BelowLen + D0 * Fog_HeightTerm(Falloff, BaseH, Ya) / FVy;
    }
    // Level or descending rays never leave the medium, so a capped segment stands in for infinity.
    return Fog_OpticalDepth(Fog, StartPos, RayDir, 100000.0);
}

// Cornette-Shanks: a smoother, energy-normalized HG. Same `G` control, but the
// lobe isn't as much of a needle.
float Fog_PhaseCornetteShanks(float CosTheta, float G)
{
    float G2  = G * G;
    float Num = 3.0 * (1.0 - G2) * (1.0 + CosTheta * CosTheta);
    float Den = 8.0 * PI * (2.0 + G2) * pow(max(1.0 + G2 - 2.0 * G * CosTheta, 1e-4), 1.5);
    return Num / Den;
}

// Two-lobe phase: forward Cornette-Shanks lobe blended with an isotropic floor so side-on shafts
// stay visible (a single high-G lobe goes near-black off-axis) while the sun-facing glow remains.
float Fog_Phase(float CosTheta, float G)
{
    const float Isotropic = 1.0 / (4.0 * PI);
    return lerp(Isotropic, Fog_PhaseCornetteShanks(CosTheta, G), 0.5);
}

// Multiple-scattering octave approximation (Hillaire 2015): evaluate single-scatter N times with
// geometrically decaying energy, phase and shadow. Additive and deliberately not renormalized.
float Fog_MultiScatterPhaseShadow(FExponentialHeightFogParams Fog, float CosTheta, float G, float Shadow)
{
    int Octaves = min(int(Fog.MultiScatterParams.x + 0.5), FOG_MAX_MULTISCATTER_OCTAVES);
    if (Octaves <= 1)
    {
        return Fog_Phase(CosTheta, G) * Shadow;
    }

    float ScatterAtten = saturate(Fog.MultiScatterParams.y);
    float ShadowAtten  = saturate(Fog.MultiScatterParams.z);
    float PhaseAtten   = saturate(Fog.MultiScatterParams.w);

    float Sum = 0.0;
    float Aw  = 1.0;   // a^n, energy
    float Bw  = 1.0;   // b^n, shadow correlation
    float Cw  = 1.0;   // c^n, phase anisotropy

    for (int n = 0; n < Octaves; ++n)
    {
        Sum += Aw * Fog_Phase(CosTheta, G * Cw) * lerp(1.0, Shadow, Bw);
        Aw *= ScatterAtten;
        Bw *= ShadowAtten;
        Cw *= PhaseAtten;
    }

    return Sum;
}

// Density ramp for FogStartDistance: fog fades in over the half-start-distance after the start,
// instead of a hard front. Dist is distance from the camera along the ray.
float Fog_StartRamp(FExponentialHeightFogParams Fog, float Dist)
{
    float Start = Fog.HeightParams.z;
    return Start > 0.0 ? saturate((Dist - Start) / max(Start * 0.5, 1.0)) : 1.0;
}

// CosSun is dot(sun travel direction, point-to-camera), so +1 means looking straight at the sun.
float3 Fog_AlbedoFrom(FExponentialHeightFogParams Fog, float3 BaseAlbedo, float CosSun, float Dist)
{
    float DirStart = Fog.VolumetricParams.w;
    float Ramp     = DirStart > 0.0 ? saturate(Dist / DirStart - 1.0) : 1.0;
    float Lobe     = pow(saturate(CosSun), Fog.DirectionalColor.w) * Ramp;
    return lerp(BaseAlbedo, Fog.DirectionalColor.rgb, Lobe);
}

float3 Fog_Albedo(FExponentialHeightFogParams Fog, float CosSun, float Dist)
{
    return Fog_AlbedoFrom(Fog, Fog.InscatteringColor.rgb, CosSun, Dist);
}

// No texture, so the froxel inject and the apply pass agree bit-for-bit without a shared bind.
float Fog_Hash(float3 P)
{
    P = frac(P * 0.3183099 + float3(0.1, 0.2, 0.3));
    P *= 17.0;
    return frac(P.x * P.y * P.z * (P.x + P.y + P.z));
}

float Fog_ValueNoise(float3 P)
{
    float3 I = floor(P);
    float3 F = frac(P);
    F = F * F * (3.0 - 2.0 * F);

    float N000 = Fog_Hash(I + float3(0.0, 0.0, 0.0));
    float N100 = Fog_Hash(I + float3(1.0, 0.0, 0.0));
    float N010 = Fog_Hash(I + float3(0.0, 1.0, 0.0));
    float N110 = Fog_Hash(I + float3(1.0, 1.0, 0.0));
    float N001 = Fog_Hash(I + float3(0.0, 0.0, 1.0));
    float N101 = Fog_Hash(I + float3(1.0, 0.0, 1.0));
    float N011 = Fog_Hash(I + float3(0.0, 1.0, 1.0));
    float N111 = Fog_Hash(I + float3(1.0, 1.0, 1.0));

    return lerp(lerp(lerp(N000, N100, F.x), lerp(N010, N110, F.x), F.y),
                lerp(lerp(N001, N101, F.x), lerp(N011, N111, F.x), F.y), F.z);
}

// Fractal sum normalized to [0,1] so the octave count changes detail without changing mean density.
float Fog_FBM(float3 P, int Octaves, float Gain)
{
    float Sum   = 0.0;
    float Amp   = 1.0;
    float Total = 0.0;
    for (int i = 0; i < Octaves; ++i)
    {
        Sum   += Amp * Fog_ValueNoise(P);
        Total += Amp;
        Amp   *= Gain;
        P     *= 2.02;
    }
    return Sum / max(Total, 1e-4);
}

// Faded out with distance because the closed-form far field cannot reproduce noise and would seam.
float Fog_NoiseScale(FExponentialHeightFogParams Fog, float3 WorldPos, float Time, float CamDist)
{
    float Strength = Fog.NoiseParams.x;
    if (Strength <= 0.0)
    {
        return 1.0;
    }

    float FadeEnd = max(Fog.NoiseWind.w, 1.0);
    float Fade    = 1.0 - saturate(CamDist / FadeEnd);
    if (Fade <= 0.0)
    {
        return 1.0;
    }

    int   Octaves = clamp(int(Fog.NoiseParams.z + 0.5), 1, 4);
    float3 P      = (WorldPos - Fog.NoiseWind.xyz * Time) * Fog.NoiseParams.y;
    float  N      = Fog_FBM(P, Octaves, saturate(Fog.NoiseParams.w));

    // Centered on 1 so raising the strength adds contrast without changing the average density.
    return max(1.0 + (N * 2.0 - 1.0) * Strength * Fade, 0.0);
}

// Contribution of the artist-placed volumes at a point, on top of the global height fog.
struct FFogMedium
{
    float  Extinction;
    float3 ScatterAlbedo;
    float3 Emissive;
    float  ScatterIntensity;
};

// Soft-edged coverage of one volume at a point, 1 in the core and 0 outside.
float Fog_VolumeCoverage(FFogVolume V, float3 WorldPos)
{
    float3 L = mul(V.WorldToVolume, float4(WorldPos, 1.0)).xyz;
    float  Softness = clamp(V.Params.y, 0.001, 1.0);

    // Sphere measures radially; box takes the dominant axis so corners do not bulge.
    float D = V.Params.x > 0.5 ? length(L) : max(abs(L.x), max(abs(L.y), abs(L.z)));
    return 1.0 - smoothstep(1.0 - Softness, 1.0, D);
}

// Split so the caller owns the volume pointer and the loop bound; only the froxel inject binds them.
FFogMedium Fog_BeginMedium(FExponentialHeightFogParams Fog, float3 WorldPos, float Time, float CamDist)
{
    FFogMedium M;
    M.Extinction       = Fog_DensityAt(Fog, WorldPos) * Fog_NoiseScale(Fog, WorldPos, Time, CamDist);
    M.ScatterAlbedo    = Fog.InscatteringColor.rgb * M.Extinction;
    M.Emissive         = float3(0.0);
    M.ScatterIntensity = M.Extinction;
    return M;
}

void Fog_AddVolume(inout FFogMedium M, FFogVolume V, float3 WorldPos)
{
    float Cover = Fog_VolumeCoverage(V, WorldPos);
    if (Cover <= 0.0)
    {
        return;
    }

    float Added = V.Albedo.w * Cover;
    M.Extinction       += Added;
    M.ScatterAlbedo    += V.Albedo.rgb * Added;
    M.Emissive         += V.Emissive.rgb * Cover;
    M.ScatterIntensity += Added * V.Params.z;
}

// Albedo and intensity were accumulated weighted by each contributor's extinction, so divide it back out.
void Fog_FinalizeMedium(inout FFogMedium M)
{
    float Inv = 1.0 / max(M.Extinction, 1e-6);
    M.ScatterAlbedo    *= Inv;
    M.ScatterIntensity *= Inv;
}

// Premultiplied in-scatter in rgb, surviving transmittance in a, for surfaces the composite misses.
float4 Fog_EvaluateAtPoint(float3 CamPos, float3 RayDir, float ViewDist, float ZPerT, float2 PixelPos, float2 ScreenUV)
{
    FExponentialHeightFogParams uFog = Scene().FogParams;
    if (Scene().bFogEnabled == 0u)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 InScatter = float3(0.0);
    float  Trans     = 1.0;

    // Reuse the already-integrated froxel volume for the near range rather than re-marching it.
    uint VolumeIndex = Scene().FogIntegratedIndex;
    if (VolumeIndex != 0xFFFFFFFFu)
    {
        float W = Froxel_DistanceToW(ViewDist, Scene().FogNearPlane, Scene().FogRange);
        W = saturate(W + (InterleavedGradientNoise(PixelPos) - 0.5) / float(max(Scene().FogGridZ, 1u)));

        float4 Froxel = gTextures3D[VolumeIndex].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], float3(ScreenUV, W), 0.0);
        InScatter = Froxel.rgb;
        Trans     = saturate(Froxel.a);
    }

    float VolEndZ = VolumeIndex != 0xFFFFFFFFu ? Scene().FogRange : 0.0;
    float T0      = max(VolEndZ / ZPerT, uFog.HeightParams.z);
    float T1      = ViewDist / ZPerT;

    if (T1 > T0)
    {
        float3 ToCam  = -RayDir;
        float3 Light  = GetVolumetricAmbientRadiance();
        float  CosSun = -1.0;
        bool   bHasSun = GetNumLights() > 0 && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional);
        if (bHasSun)
        {
            FLight Sun = GetLightAt(0);
            CosSun = dot(-normalize(Sun.Direction), ToCam);
            if (HasFlag(Sun.Flags, ELightFlags::Volumetric))
            {
                Light += UnpackColor(Sun.Color).rgb * Sun.Intensity
                       * Fog_MultiScatterPhaseShadow(uFog, CosSun, uFog.VolumetricParams.y, 1.0)
                       * Sun.VolumetricIntensity;
            }
        }

        float3 P0 = CamPos + RayDir * T0;
        float  OD = Fog_OpticalDepth(uFog, P0, RayDir, T1 - T0) * Fog_StartRamp(uFog, 0.5 * (T0 + T1));
        float  TransA = exp(-OD);

        float3 Albedo = Fog_Albedo(uFog, CosSun, 0.5 * (T0 + T1));
        InScatter += Trans * Albedo * Light * uFog.VolumetricParams.x * (1.0 - TransA);
        Trans     *= TransA;
    }

    float Opacity = 1.0 - Trans;
    float Clamped = min(Opacity, uFog.HeightParams.w);
    InScatter    *= Opacity > 1e-4 ? Clamped / Opacity : 0.0;
    return float4(InScatter, 1.0 - Clamped);
}

// Fog between the camera and a view-space point, for surfaces the fullscreen composite cannot reach.
float4 Fog_EvaluateForTranslucent(float3 ViewPos, float2 PixelPos)
{
    float3 CamPos   = GetCameraPosition();
    float3 WorldPos = mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;
    float3 Delta    = WorldPos - CamPos;
    float  Len      = length(Delta);
    if (Len < 1e-4)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float  ViewDist = max(ViewPos.z, Scene().FogNearPlane);
    float3 RayDir   = Delta / Len;
    float  ZPerT    = max(ViewDist / Len, 1e-4);
    float2 ScreenUV = PixelPos / float2(Scene().ScreenSize.xy);

    return Fog_EvaluateAtPoint(CamPos, RayDir, ViewDist, ZPerT, PixelPos, ScreenUV);
}
