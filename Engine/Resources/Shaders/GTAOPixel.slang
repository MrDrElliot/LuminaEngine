#pragma once
#include "Includes/SceneGlobals.slang"

// GTAO (Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate Indirect Occlusion"):
// per pixel, a few hemisphere slices march the depth buffer for horizon angles and integrate the
// cosine-weighted visible arc analytically. Spatial-only (IGN rotation per pixel, no noise texture,
// no temporal accumulation); the matching bilateral blur removes the slice noise. Runs at half res.
// View space is +Z forward; output is one grayscale visibility factor in [0,1] (1 = unoccluded).

#define GTAO_SLICES          3
#define GTAO_STEPS_PER_SIDE  6

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

struct FData
{
    uint DepthIndex;   // bindless SRV of the full-res scene depth buffer
};

float SampleDepth(uint DepthIndex, float2 UV)
{
    return gTextures2D[DepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, 0.0).r;
}

// Interleaved gradient noise: stable per-pixel spatial rotation, no texture needed.
float IGN(float2 PixelPos)
{
    return frac(52.9829189 * frac(dot(PixelPos, float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    FData* Data = GetPassArgs<FData>();

    float4x4 InvProj = GetInverseCameraProjection();

    // Half-res pixel centers land exactly on full-res depth texel CORNERS, so an unsnapped point
    // sample flip-flops between neighbouring texels per row/column and the whole pass inherits a
    // scanline pattern. Snap to the nearest full-res texel center first.
    float2 FullTexel = 1.0 / float2(Scene().ScreenSize.xy);
    float2 UV = (floor(Input.UV * float2(Scene().ScreenSize.xy)) + 0.5) * FullTexel;

    float DepthC = SampleDepth(Data.DepthIndex, UV);

    // Reverse-Z: the far plane / sky is 0. Nothing to occlude there -> fully lit.
    if (DepthC <= 0.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    float3 P = ReconstructViewPos(UV, DepthC, InvProj);
    float3 V = normalize(-P);   // toward the camera

    // Normal from depth: side-picked derivatives (use the neighbour with the smaller depth delta
    // per axis) so normals stay clean at depth edges instead of smearing across silhouettes.
    float  DL = SampleDepth(Data.DepthIndex, UV - float2(FullTexel.x, 0.0));
    float  DR = SampleDepth(Data.DepthIndex, UV + float2(FullTexel.x, 0.0));
    float  DU = SampleDepth(Data.DepthIndex, UV - float2(0.0, FullTexel.y));
    float  DD = SampleDepth(Data.DepthIndex, UV + float2(0.0, FullTexel.y));

    float3 dPdx, dPdy;
    if (abs(DR - DepthC) < abs(DL - DepthC))
    {
        dPdx = ReconstructViewPos(UV + float2(FullTexel.x, 0.0), DR, InvProj) - P;
    }
    else
    {
        dPdx = P - ReconstructViewPos(UV - float2(FullTexel.x, 0.0), DL, InvProj);
    }
    if (abs(DD - DepthC) < abs(DU - DepthC))
    {
        dPdy = ReconstructViewPos(UV + float2(0.0, FullTexel.y), DD, InvProj) - P;
    }
    else
    {
        dPdy = P - ReconstructViewPos(UV - float2(0.0, FullTexel.y), DU, InvProj);
    }
    float3 N = normalize(cross(dPdy, dPdx));
    if (dot(N, V) < 0.0)
    {
        N = -N;
    }

    float Radius = Scene().GTAOSettings.Radius;

    // World radius -> UV-space march radius at this depth. abs() on [1][1]: the
    // projection bakes the Vulkan Y-flip.
    float  ProjScale  = abs(GetCameraProjection()[1][1]);
    float  RadiusUV   = Radius * ProjScale * 0.5 / max(P.z, 0.05);
    // Clamp the footprint so very near surfaces don't march the whole screen.
    RadiusUV = min(RadiusUV, 0.25);

    // Too far for the radius to span even one depth texel: nothing to measure.
    float MaxTexel = max(FullTexel.x, FullTexel.y);
    if (RadiusUV < MaxTexel)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    // Never march closer than ~1.5 depth texels: a same-texel tap yields a degenerate
    // near-zero-length direction that reads as patterned self-occlusion on flat surfaces.
    float TMin = saturate(1.5 * MaxTexel / RadiusUV);

    float Noise     = IGN(Input.Position.xy);
    // Phase-shifted IGN so the step jitter doesn't share the slice rotation's spatial pattern.
    float StepNoise = IGN(Input.Position.xy + float2(47.0, 17.0));
    // Distance falloff so occlusion fades smoothly at the radius edge.
    float FalloffMul = -1.0 / max(Radius, 1e-4);
    // Horizon-cos guard against depth-precision self-occlusion on flat surfaces.
    float HorizonBias = Scene().GTAOSettings.Bias;

    float Visibility = 0.0;

    // [loop] (not [unroll]): unrolling replicates the inner step march -- each copy holds the 4x4 InvProj +
    // ReconstructViewPos temporaries live -- which is the GTAO register dominator. Rolling keeps one slice's worth.
    [loop]
    for (int SliceI = 0; SliceI < GTAO_SLICES; ++SliceI)
    {
        float  Phi  = (float(SliceI) + Noise) * (PI / float(GTAO_SLICES));
        float  CosP = cos(Phi);
        float  SinP = sin(Phi);

        // Screen march direction. View +Y maps to UV -Y (Vulkan Y-flip), hence the negated y.
        float2 Omega = float2(CosP, -SinP) * RadiusUV;

        // Slice basis: project N onto the plane spanned by V and the slice direction.
        float3 DirV     = float3(CosP, SinP, 0.0);
        float3 OrthoDir = DirV - V * dot(DirV, V);
        float3 Axis     = normalize(cross(OrthoDir, V));
        float3 ProjN    = N - Axis * dot(N, Axis);
        float  ProjNLen = length(ProjN);
        if (ProjNLen < 1e-4)
        {
            Visibility += 1.0;   // normal ~parallel to the slice axis: nothing to occlude in-plane
            continue;
        }

        float CosNorm = saturate(dot(ProjN, V) / ProjNLen);
        float Gamma   = sign(dot(OrthoDir, ProjN)) * acos(CosNorm);   // signed angle of ProjN from V

        // March both sides for the max horizon cosine.
        float2 HorizonCos = float2(-1.0, -1.0);
        // [loop]: the per-step ReconstructViewPos (4x4 InvProj live) is the GTAO register dominator; rolling
        // collapses GTAO_STEPS_PER_SIDE live tap-sets to one. Extra resident warps re-hide the depth-tap latency.
        [loop]
        for (int StepI = 0; StepI < GTAO_STEPS_PER_SIDE; ++StepI)
        {
            // Squared distribution (more samples near the center, where occlusion detail lives),
            // remapped to start at the minimum texel offset.
            float T = (float(StepI) + StepNoise) / float(GTAO_STEPS_PER_SIDE);
            T = lerp(TMin, 1.0, T * T);

            [unroll]
            for (int Side = 0; Side < 2; ++Side)
            {
                float2 SUV = UV + (Side == 0 ? Omega : -Omega) * T;
                float  SD  = SampleDepth(Data.DepthIndex, SUV);
                if (SD <= 0.0 || any(saturate(SUV) != SUV))
                {
                    continue;
                }

                float3 SP   = ReconstructViewPos(SUV, SD, InvProj);
                float3 Diff = SP - P;
                float  Dist = length(Diff);
                if (Dist < 1e-3)
                {
                    continue;   // same surface point: no horizon information
                }
                float  CosH = dot(Diff, V) / Dist - HorizonBias;

                // Fade samples out toward the radius so AO doesn't hard-cut, and reject far
                // background (thin-object heuristic: distant geometry can't be this surface's occluder).
                float Falloff = saturate(Dist * FalloffMul + 1.0);
                CosH = lerp(-1.0, CosH, Falloff);

                if (Side == 0) { HorizonCos.x = max(HorizonCos.x, CosH); }
                else           { HorizonCos.y = max(HorizonCos.y, CosH); }
            }
        }

        // Horizon angles relative to V, clamped to the hemisphere around the projected normal.
        float H0 = -acos(clamp(HorizonCos.y, -1.0, 1.0));
        float H1 =  acos(clamp(HorizonCos.x, -1.0, 1.0));
        H0 = Gamma + max(H0 - Gamma, -PI * 0.5);
        H1 = Gamma + min(H1 - Gamma,  PI * 0.5);

        // Analytic cosine-weighted arc integral (the "ground truth" part of GTAO).
        float Arc0 = -cos(2.0 * H0 - Gamma) + CosNorm + 2.0 * H0 * sin(Gamma);
        float Arc1 = -cos(2.0 * H1 - Gamma) + CosNorm + 2.0 * H1 * sin(Gamma);
        Visibility += ProjNLen * 0.25 * (Arc0 + Arc1);
    }

    Visibility = saturate(Visibility / float(GTAO_SLICES));

    float AO = saturate(1.0 - (1.0 - Visibility) * Scene().GTAOSettings.Intensity);
    AO = pow(AO, Scene().GTAOSettings.Power);

    return float4(AO, AO, AO, 1.0);
}
