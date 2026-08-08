#pragma once

// GPU mirror of FExponentialHeightFogParams (EnvironmentRenderTypes.h). One medium definition shared
// by the froxel volumetric passes (near range, shadowed) and the analytic height-fog composite
// (beyond the froxel range + sky), so both agree on density, albedo, and lighting and the hand-off
// at the froxel far plane is seamless.
struct FExponentialHeightFogParams
{
    float4 InscatteringColor;   // rgb = fog albedo, w = density at base height
    float4 HeightParams;        // x = falloff, y = baseHeight, z = startDistance, w = maxOpacity
    float4 DirectionalColor;    // rgb = sun inscatter tint, w = exponent
    float4 VolumetricParams;    // x = scatterIntensity, y = anisotropy, z = froxel range, w = dirStartDistance
    // x = multi-scatter octave count (<= 1 disables), y = per-octave scattering attenuation,
    // z = per-octave shadow attenuation, w = per-octave phase attenuation. See Fog_MultiScatterPhaseShadow.
    float4 MultiScatterParams;
};

// Hard ceiling on the octave loop. Mirrors the ClampMax on the component property; repeated here so a
// malformed upload can't run the loop unbounded inside the per-light path.
static const int FOG_MAX_MULTISCATTER_OCTAVES = 4;

// Local fog density at a world-space point: exponential falloff with height.
float Fog_DensityAt(FExponentialHeightFogParams Fog, float3 WorldPos)
{
    float Falloff = Fog.HeightParams.x;
    float BaseH   = Fog.HeightParams.y;
    return Fog.InscatteringColor.w * exp(-Falloff * (WorldPos.y - BaseH));
}

// Closed-form optical depth of the exp-height medium along a ray segment (StartPos, unit RayDir,
// length SegLen): analytic integral of density * exp(-falloff * (y - baseHeight)).
float Fog_OpticalDepth(FExponentialHeightFogParams Fog, float3 StartPos, float3 RayDir, float SegLen)
{
    float Falloff   = Fog.HeightParams.x;
    float BaseH     = Fog.HeightParams.y;
    float BaseDens  = Fog.InscatteringColor.w * exp(clamp(-Falloff * (StartPos.y - BaseH), -60.0, 60.0));
    float Vy        = RayDir.y;

    // For near-horizontal rays the height term is ~constant over the segment;
    // fall back to the rectangular integral to avoid a divide-by-tiny blowup.
    if (abs(Falloff * Vy) > 1e-5)
    {
        // Clamp the exponent so steep downward rays stay finite (the medium densifies below).
        return BaseDens * (1.0 - exp(clamp(-Falloff * Vy * SegLen, -60.0, 60.0))) / (Falloff * Vy);
    }
    return BaseDens * SegLen;
}

// Optical depth from StartPos to infinity along the ray (sky pixels). Converges for rays that
// climb out of the medium; level/descending rays are treated as a long capped segment.
float Fog_OpticalDepthToInfinity(FExponentialHeightFogParams Fog, float3 StartPos, float3 RayDir)
{
    float Falloff  = Fog.HeightParams.x;
    float BaseH    = Fog.HeightParams.y;
    float BaseDens = Fog.InscatteringColor.w * exp(clamp(-Falloff * (StartPos.y - BaseH), -60.0, 60.0));
    float FVy      = Falloff * RayDir.y;

    if (FVy > 1e-5)
    {
        return BaseDens / FVy;
    }
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

// Higher-order (multiple) scattering, octave approximation -- Hillaire, "Physically Based and Unified
// Volumetric Rendering in Frostbite" (2015).
//
// Single scattering only delivers light that reached a froxel unobstructed and bounced exactly once,
// and a dense medium is precisely where that stops being most of the light. The two failures are both
// visible: shadowed fog collapses onto the flat ambient constant (no shape, no gradient -- the "flat
// gray" look), and the sun lobe stays needle-sharp no matter how thick the fog gets, when a thick
// medium should be washing it out. Real media redistribute light over many bounces, which fills
// shadow and broadens the lobe together.
//
// Instead of marching secondary bounces, evaluate the SAME single-scatter term N times with
// geometrically decaying parameters, each octave standing in for one further bounce:
//
//   octave n:  energy  *= a^n              absorption takes a cut at every bounce
//              phase g *= c^n              each bounce randomizes direction, flattening toward isotropic
//              shadow  -> lerp(1, S, b^n)  each bounce is less correlated with the direct light path,
//                                          so later octaves stop respecting the shadow map and leak in
//
// That shadow term is where this departs from the paper, deliberately. Frostbite scales EXTINCTION by
// b^n, lengthening the mean free path per octave along both the light and view rays. Neither is
// available to us: shadow maps give visibility, not optical depth along the light path, and the
// view-ray extinction is baked once into the scatter volume's alpha for VolumetricFogIntegrate --
// per-octave extinction there would need N volumes and N integrations. Softening the visibility term
// instead reproduces what b^n exists to produce (light bleeding into shadow as bounces decorrelate)
// for pure ALU on a value already in hand. That trade is the right way round for this pass: the
// shadow taps are what it costs, not the arithmetic, and this adds none.
//
// Returns the combined phase * shadow weight and replaces `Fog_Phase(...) * Shadow` at every call
// site, so the froxel inject and the analytic far-field composite in VolumetricFogApply stay on one
// lighting model and the hand-off at the froxel far plane remains seamless. The far field passes
// Shadow = 1, which makes the leak term identity and leaves only the energy and phase octaves --
// exactly what the froxel volume converges to in unshadowed regions, so the seam is continuous by
// construction rather than by tuning.
//
// Octaves are ADDITIVE and deliberately not renormalized: multiple scattering genuinely carries more
// energy than the single-scatter estimate, which under-counts it. Raising the octave count therefore
// brightens the fog on purpose; VolumetricScatteringIntensity is the knob to trim it back.
//
// At Octaves <= 1 this returns Fog_Phase(CosTheta, G) * Shadow bit for bit, so the feature is a true
// no-op (and costs one compare) until a scene opts into it.
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

// Fog albedo with the sun-facing directional inscatter tint: lerped toward DirectionalColor by a
// forward pow-lobe, ramped in past the directional start distance. CosSun is dot(sun travel
// direction, point-to-camera direction): +1 when looking straight at the sun.
float3 Fog_Albedo(FExponentialHeightFogParams Fog, float CosSun, float Dist)
{
    float DirStart = Fog.VolumetricParams.w;
    float Ramp     = DirStart > 0.0 ? saturate(Dist / DirStart - 1.0) : 1.0;
    float Lobe     = pow(saturate(CosSun), Fog.DirectionalColor.w) * Ramp;
    return lerp(Fog.InscatteringColor.rgb, Fog.DirectionalColor.rgb, Lobe);
}
