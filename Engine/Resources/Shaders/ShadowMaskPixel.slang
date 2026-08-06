#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/ShadowSampling.slang"

// Screen-space sun-shadow mask ("deferred shadow projection"): resolve the directional cascade PCSS
// once per screen pixel over the opaque depth buffer, into a single R8 visibility channel that the
// opaque shading passes then read with one texel fetch.
//
// This is not a cost optimization -- it is the same PCSS over roughly the same pixels. It is a
// REGISTER PRESSURE optimization. Whole-shader VGPR allocation reserves the peak live range across
// every path in a shader, so the cascade PCSS (two inlined SampleDirectionalCascade bodies for the
// cascade cross-fade, each holding a 4x4 cascade VP, the receiver-plane Jacobian, an atlas tile
// descriptor and a rolling tap loop) set the register floor for the ENTIRE deferred material shader --
// material graph, clustered light loop, IBL and all -- including for pixels that never touch the sun.
// Measured symptom: PS Warp Launch Stalled Register Allocation at 91.4%, 13.68 active warps per cycle.
//
// Here the same code is the whole shader. It has nothing else to be live alongside, so it allocates
// few registers, reaches high occupancy, and the extra resident warps hide its own texture latency.
//
// Still a PIXEL shader rather than compute, deliberately: SampleDirectionalCascade derives the
// receiver-plane depth slope from ddx/ddy of the shadow-space UV and depth. A compute port would need
// quad intrinsics or a hand-rolled finite difference, i.e. a SECOND copy of the PCSS that can drift
// from the one ShadeSurface still uses for translucency and captures. One copy, two call sites.

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

struct FData
{
    uint DepthIndex;   // bindless SRV of the full-res opaque scene depth buffer
};

float SampleDepth(uint DepthIndex, float2 UV)
{
    return gTextures2D[DepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, 0.0).r;
}

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    FData* Data = GetPassArgs<FData>();

    // No sun, or the sun casts no shadows: everything is lit. The pass is skipped entirely in that
    // case (the scene root keeps the ~0u sentinel), so this is only a safety net.
    if (LightData().bHasSun == 0u)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    float4x4 InvProj = GetInverseCameraProjection();

    // Full res, so pixel centers already land on depth texel centers -- no snap needed (unlike SSAO,
    // which runs at half res).
    float2 FullTexel = 1.0 / float2(Scene().ScreenSize.xy);
    float2 UV        = Input.UV;

    float DepthC = SampleDepth(Data.DepthIndex, UV);

    // Reverse-Z: the far plane / sky is 0. Nothing there receives a shadow.
    if (DepthC <= 0.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    float3 P = ReconstructViewPos(UV, DepthC, InvProj);

    // GEOMETRIC normal from depth, side-picked per axis (take the neighbour with the smaller depth
    // delta) so the frame stays on one surface at silhouettes instead of smearing across them. Same
    // construction as SSAOPixel.
    //
    // ComputeDirectionalShadow uses the normal for exactly two things -- the slope term of the depth
    // bias and the normal-offset push -- and for BOTH the geometric normal is the right input. The
    // inline path fed it the normal-MAPPED shading normal, which is arguably wrong: normal-offset bias
    // corrects shadow-map texel quantization on the surface the shadow pass actually rasterized, and a
    // normal map does not move that surface. Expect this to reduce, not add, acne on bumpy materials.
    float DL = SampleDepth(Data.DepthIndex, UV - float2(FullTexel.x, 0.0));
    float DR = SampleDepth(Data.DepthIndex, UV + float2(FullTexel.x, 0.0));
    float DU = SampleDepth(Data.DepthIndex, UV - float2(0.0, FullTexel.y));
    float DD = SampleDepth(Data.DepthIndex, UV + float2(0.0, FullTexel.y));

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

    float3 NView = normalize(cross(dPdy, dPdx));
    if (dot(NView, normalize(-P)) < 0.0)
    {
        NView = -NView;
    }

    // The view matrix is rigid, so its inverse's upper-left 3x3 is the (orthonormal) camera basis:
    // rotating the view-space normal by it is exact, no inverse-transpose needed.
    float4x4 InvView   = GetInverseCameraView();
    float3   WorldPos  = mul(InvView, float4(P, 1.0)).xyz;
    float3   NWorld    = normalize(mul((float3x3)InvView, NView));

    // No NdotL early-out for sun-facing-away pixels, even though ShadeSurface has one and this
    // therefore resolves ~2x the pixels the inline path did. A depth-derived normal is unreliable
    // exactly at silhouettes, and a wrongly-skipped pixel writes 1.0 -- a bright speckle on a shadowed
    // edge, which is a worse failure than doing the work. Revisit only if this pass measures hot.
    FLight Sun  = GetLightAt(0);
    float  Mask = ComputeDirectionalShadow(Sun, WorldPos, NWorld, P, Input.Position.xy);

    // Per-instance ReceiveShadow and the material's own parallax SelfShadow are NOT folded in here:
    // both are surface properties this pass cannot see from depth, and both stay a cheap multiply /
    // select in ShadeSurface where the values are already live.
    return float4(Mask, Mask, Mask, 1.0);
}
