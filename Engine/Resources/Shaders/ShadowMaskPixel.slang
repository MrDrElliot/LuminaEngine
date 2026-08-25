#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/ShadowSampling.slang"

// Screen-space sun-shadow mask. A REGISTER PRESSURE optimization, not a cost one: inline, the cascade
// PCSS set the VGPR floor for the whole deferred shader. Still a PIXEL shader -- the bias needs ddx/ddy.

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


// Screen-space fill for occlusion below one cascade texel, which is what makes contact points float. 1 = lit.
float ContactShadow(uint DepthIndex, float4x4 InvProj, float3 ViewPos, float3 ViewN, float3 ViewL, float Jitter)
{
    const float  Reach = LightData().ShadowParams2.z;
    const uint   Steps = (uint)LightData().ShadowParams2.w;
    if (Reach <= 0.0 || Steps == 0u)
    {
        return 1.0;
    }

    const float3 Step = ViewL * (Reach / float(Steps));

    // Half-step jittered start plus a normal offset, or a grazing surface reports its own ray as occluding.
    float3 RayPos = ViewPos + ViewN * (Reach / float(Steps)) + Step * (0.5 + Jitter);

    // A hit farther behind the ray than this is a separate surface, not an occluder in contact.
    const float Thickness = Reach;

    for (uint i = 0u; i < Steps; ++i)
    {
        const float4 Clip = mul(GetCameraProjection(), float4(RayPos, 1.0));
        if (Clip.w <= 0.0)
        {
            break;
        }

        const float2 UV = (Clip.xy / Clip.w) * 0.5 + 0.5;
        if (any(saturate(UV) != UV))
        {
            break;   // left the depth buffer, and off-screen occluders are not knowable here
        }

        const float Raw = SampleDepth(DepthIndex, UV);
        if (Raw > 0.0)
        {
            const float SceneZ = ReconstructViewPos(UV, Raw, InvProj).z;
            const float Delta  = RayPos.z - SceneZ;   // positive = the ray is behind the depth buffer

            if (Delta > 0.0 && Delta < Thickness)
            {
                // Fade out along the ray so the far end of the reach does not cut a hard line.
                return saturate(float(i) / float(Steps));
            }
        }

        RayPos += Step;
    }

    return 1.0;
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

    // Full res, so pixel centers already land on depth texel centers -- no snap needed (unlike GTAO,
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

    // GEOMETRIC normal from depth, side-picked per axis so the frame stays on one surface at silhouettes.
    // Geometric is the right input here: normal-offset bias corrects the surface the shadow pass rasterized.
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

    // No NdotL early-out: a depth-derived normal is unreliable exactly at silhouettes, and a wrongly
    // skipped pixel writes 1.0 -- a bright speckle on a shadowed edge is worse than doing the work.
    FLight Sun  = GetLightAt(0);
    float  Mask = ComputeDirectionalShadow(Sun, WorldPos, NWorld, P, Input.Position.xy);

    // min rather than a multiply, which would darken a PCSS penumbra twice for the one occluder.
    const float3 ViewL  = normalize(NormalWorldToView(normalize(Sun.Direction.xyz)));
    const float  Jitter = frac(52.9829189 * frac(dot(Input.Position.xy, float2(0.06711056, 0.00583715))));
    Mask = min(Mask, ContactShadow(Data.DepthIndex, InvProj, P, NView, ViewL, Jitter));

    // Per-instance ReceiveShadow and the material's parallax SelfShadow are NOT folded in: this pass
    // cannot see them from depth, and both stay a cheap multiply in ShadeSurface where they are live.
    return float4(Mask, Mask, Mask, 1.0);
}
