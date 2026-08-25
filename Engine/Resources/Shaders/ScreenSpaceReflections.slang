// Compute lane: no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHIStorage.slang"

// Swaps the prefiltered environment radiance for a traced one wherever the screen has the answer.

struct FSSRArgs
{
    uint  GBufferAIndex;
    uint  GBufferBIndex;
    uint  GBufferCIndex;
    uint  GBufferDIndex;

    uint  DepthIndex;
    uint  HDRUAV;          // bindless 2D UAV: HDR, the composite target
    uint  SceneColorIndex; // bindless 2D SRV: pre-pass copy of HDR, the trace reads this
    uint  ScreenW;

    uint  ScreenH;
    uint  MaxSteps;
    float MaxDistance;
    float Thickness;       // view-space depth span a hit may straddle before it reads as an occluder

    float Intensity;
    float RoughnessFade;   // roughness at which SSR has fully handed back to the cube
    uint  _Pad0;
    uint  _Pad1;

    uint* PixelList;
    uint* Total;
};
FSSRArgs* Pass() { return GetPassArgs<FSSRArgs>(); }

// Spatially stable per-pixel hash; jitters the ray start so step banding breaks into fine grain.
float SSR_IGN(float2 PixelPos)
{
    return InterleavedGradientNoise(PixelPos);
}

float SSR_ViewDepthAt(float2 UV)
{
    float Raw = SampleTexture2DLevel(Pass().DepthIndex, SAMPLER_POINT_CLAMP, UV, 0.0).r;
    if (Raw <= 0.0)
    {
        return GetFarPlane();
    }
    return ReconstructViewPos(UV, Raw, GetInverseCameraProjection()).z;
}

[shader("compute")]
[numthreads(MATERIAL_PIXEL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    const uint LinearGroup = GroupID.y * MAX_DISPATCH_AXIS + GroupID.x;
    const uint Index       = LinearGroup * MATERIAL_PIXEL_GROUP_SIZE + Lane;
    if (Index >= Pass().Total[0])
    {
        return;
    }

    const uint2 Texel = UnpackVisPixel(Pass().PixelList[Index]);

    FGBufferTargets Targets;
    Targets.A = Pass().GBufferAIndex;
    Targets.B = Pass().GBufferBIndex;
    Targets.C = Pass().GBufferCIndex;
    Targets.D = Pass().GBufferDIndex;

    const FGBufferData G = LoadGBuffer(Targets, int2(Texel));
    if (IsUnlit(G))
    {
        return;
    }

    const float Roughness = max(G.Roughness, 0.045);
    const float RoughFade = 1.0 - saturate(Roughness / max(Pass().RoughnessFade, 1e-3));
    if (RoughFade <= 0.0)
    {
        return;
    }

    const float2 ScreenPos = float2(Texel) + 0.5;
    const float2 UV        = ScreenPos / float2(Pass().ScreenW, Pass().ScreenH);
    const float  RawDepth  = LoadTexture2D(Pass().DepthIndex, int2(Texel)).r;
    if (RawDepth <= 0.0)
    {
        return;
    }

    const float3 ViewPos       = ReconstructViewPos(UV, RawDepth, GetInverseCameraProjection());
    const float3 WorldPosition = mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz;

    const float3 N = G.WorldNormal;
    const float3 V = IsCameraOrthographic() ? -GetCameraForward() : normalize(GetCameraPosition() - WorldPosition);
    const float  NdotV = saturate(dot(N, V));

    const float3 R     = reflect(-V, N);
    const float3 ViewR = normalize(mul((float3x3)GetCameraView(), R));

    // View space is +Z forward; a ray heading back at the eye can never land on screen geometry.
    if (ViewR.z <= 0.001)
    {
        return;
    }

    float3 StartV = ViewPos;
    float3 EndV   = ViewPos + ViewR * Pass().MaxDistance;

    // Past the near plane the projection flips and the screen-space walk stops meaning anything.
    const float NearZ = GetNearPlane();
    if (EndV.z < NearZ)
    {
        EndV = lerp(StartV, EndV, saturate((NearZ - StartV.z) / (EndV.z - StartV.z)));
    }

    const float4 ClipS = mul(GetCameraProjection(), float4(StartV, 1.0));
    const float4 ClipE = mul(GetCameraProjection(), float4(EndV, 1.0));
    if (ClipS.w <= 1e-4 || ClipE.w <= 1e-4)
    {
        return;
    }

    const float2 ScreenSize = float2(Pass().ScreenW, Pass().ScreenH);
    const float2 PixS = ((ClipS.xy / ClipS.w) * 0.5 + 0.5) * ScreenSize;
    const float2 PixE = ((ClipE.xy / ClipE.w) * 0.5 + 0.5) * ScreenSize;

    // 1/w is what interpolates linearly across the screen, so depth is carried as its reciprocal.
    const float InvWS = 1.0 / ClipS.w;
    const float InvWE = 1.0 / ClipE.w;

    const float PixelSpan = length(PixE - PixS);
    if (PixelSpan < 1.0)
    {
        return;
    }

    // Uniform PIXEL strides: uniform world steps are many pixels up close and sub-pixel far away.
    const uint  Steps  = max(Pass().MaxSteps, 4u);
    const float Stride = max(PixelSpan / float(Steps), 1.0);
    const uint  Count  = max(min((uint)(PixelSpan / Stride), Steps), 1u);
    const float Jitter = SSR_IGN(ScreenPos);

    // Grazing rays sit within depth-buffer precision of their own surface for the first few steps.
    const float ZBias = max(0.05, ViewPos.z * 0.002);

    float  PrevT = 0.0;
    bool   bHit  = false;
    float2 HitUV = UV;

    [loop]
    for (uint s = 1u; s <= Count; ++s)
    {
        float  T   = min((float(s) + Jitter - 0.5) / float(Count), 1.0);
        float2 Suv = lerp(PixS, PixE, T) / ScreenSize;
        if (any(saturate(Suv) != Suv))
        {
            break;
        }

        float RayZ = 1.0 / lerp(InvWS, InvWE, T);

        if (RayZ > SSR_ViewDepthAt(Suv) + ZBias)
        {
            // Refine first: gating entry on thickness discards nearly every crossing that overshot.
            float Lo = PrevT;
            float Hi = T;
            [unroll]
            for (int r = 0; r < 6; ++r)
            {
                float  Mid   = (Lo + Hi) * 0.5;
                float2 MidUV = lerp(PixS, PixE, Mid) / ScreenSize;
                float  MidZ  = 1.0 / lerp(InvWS, InvWE, Mid);
                if (MidZ > SSR_ViewDepthAt(MidUV))
                {
                    Hi = Mid;
                }
                else
                {
                    Lo = Mid;
                }
            }

            float2 RefUV = lerp(PixS, PixE, Hi) / ScreenSize;
            float  RefZ  = 1.0 / lerp(InvWS, InvWE, Hi);
            if (RefZ - SSR_ViewDepthAt(RefUV) < Pass().Thickness)
            {
                HitUV = RefUV;
                bHit  = true;
            }
            break;
        }

        PrevT = T;
    }

    if (!bHit)
    {
        return;
    }

    // Screen border and eye-facing rays are where screen space has no data; a hard cut reads as a seam.
    float2 EdgeD      = abs(HitUV - 0.5) * 2.0;
    float  EdgeFade   = smoothstep(1.0, 0.85, max(EdgeD.x, EdgeD.y));
    float  BackFade   = saturate(ViewR.z * 4.0);
    float  Confidence = EdgeFade * BackFade * RoughFade * saturate(Pass().Intensity);
    if (Confidence <= 0.0)
    {
        return;
    }

    float3 HitColor = SanitizeRadiance(
        SampleTexture2DLevel(Pass().SceneColorIndex, SAMPLER_LINEAR_CLAMP, HitUV, 0.0).rgb);

    // The same terms EvaluateIBLAmbientSplit builds, so only the radiance source is being swapped.
    float3 F0     = lerp(float3(0.08 * G.Specular), G.BaseColor, G.Metallic);
    float2 DFG    = SampleBRDFLUT(NdotV, Roughness);
    float3 FssEss = FresnelSchlickRoughness(NdotV, F0, Roughness) * DFG.x + DFG.y;
    // Same occlusion the base pass gave this lobe, or the SSR coverage edge reads as a brightness step.
    float  AmbientAO = G.AmbientOcclusion;
    if (Scene().GTAOSettings.AOTextureIndex != 0xFFFFFFFFu)
    {
        AmbientAO *= SampleTexture2DLevel(Scene().GTAOSettings.AOTextureIndex, SAMPLER_LINEAR_CLAMP, UV, 0.0).r;
    }
    float  SpecAO = ComputeSpecularOcclusion(NdotV, AmbientAO, Roughness);

    float3 PreFilt = SampleSpecularRadiance(WorldPosition, R, Roughness);
    float3 Delta   = FssEss * SpecAO * (HitColor - PreFilt) * Confidence;

    float4 Dst = gRWTextures2D[Pass().HDRUAV][int2(Texel)];
    gRWTextures2D[Pass().HDRUAV][int2(Texel)] = float4(Dst.rgb + Delta, Dst.a);
}
