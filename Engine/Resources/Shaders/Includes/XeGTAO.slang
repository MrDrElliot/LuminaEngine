#pragma once
#include "SceneGlobals.slang"

// Slang port of Intel's XeGTAO (https://github.com/GameTechDev/XeGTAO), MIT, see XeGTAO.LICENSE.txt.

#define XE_GTAO_PI                      3.1415926535897932384626433832795
#define XE_GTAO_PI_HALF                 1.5707963267948966192313216916398

#define XE_GTAO_DEPTH_MIP_LEVELS        5
#define XE_GTAO_NUMTHREADS_X            8
#define XE_GTAO_NUMTHREADS_Y            8

// Pre-denoise visibility overshoots 1, so the working term is stored scaled down and restored on final apply.
#define XE_GTAO_OCCLUSION_TERM_SCALE    1.5

// A slice narrower than this many pixels resolves nothing, and the minimum step would span the screen.
#define XE_GTAO_PIXEL_TOO_CLOSE         1.3

// Probed from the engine's own inverse projection rather than assumed from a matrix layout.
struct FGTAOView
{
    float2 NDCToViewMul;
    float2 NDCToViewAdd;
    float2 ViewportPixelSize;
    float2 NDCToViewMulPixelSize;
    float4 DepthUnpack;
};

// ViewXY/Z is affine in UV under a pinhole camera, at any depth, so two corner probes pin the map.
FGTAOView MakeGTAOView(uint2 ViewportSize)
{
    const float4x4 InvProj = GetInverseCameraProjection();

    const float3 P00 = ReconstructViewPos(float2(0.0, 0.0), 0.5, InvProj);
    const float3 P11 = ReconstructViewPos(float2(1.0, 1.0), 0.5, InvProj);
    const float2 Q00 = P00.xy / P00.z;
    const float2 Q11 = P11.xy / P11.z;

    FGTAOView V;
    V.NDCToViewMul          = Q11 - Q00;
    V.NDCToViewAdd          = Q00;
    V.ViewportPixelSize     = 1.0 / float2(ViewportSize);
    V.NDCToViewMulPixelSize = V.NDCToViewMul * V.ViewportPixelSize;
    // Rows 2 and 3 hold the entire depth map, and a jittered projection leaves both untouched.
    V.DepthUnpack           = float4(InvProj[2].z, InvProj[2].w, InvProj[3].z, InvProj[3].w);
    return V;
}

float3 XeGTAO_ComputeViewspacePosition(FGTAOView V, float2 ScreenPos, float ViewspaceDepth)
{
    float3 Result;
    Result.xy = (V.NDCToViewMul * ScreenPos + V.NDCToViewAdd) * ViewspaceDepth;
    Result.z  = ViewspaceDepth;
    return Result;
}

float XeGTAO_ScreenSpaceToViewSpaceDepth(FGTAOView V, float ScreenDepth)
{
    return (V.DepthUnpack.x * ScreenDepth + V.DepthUnpack.y)
         / (V.DepthUnpack.z * ScreenDepth + V.DepthUnpack.w);
}

float XeGTAO_ClampDepth(float Depth)
{
    return clamp(Depth, 0.0, 3.402823466e+38);
}

float4 XeGTAO_CalculateEdges(float CenterZ, float LeftZ, float RightZ, float TopZ, float BottomZ)
{
    float4 EdgesLRTB = float4(LeftZ, RightZ, TopZ, BottomZ) - CenterZ;

    const float  SlopeLR       = (EdgesLRTB.y - EdgesLRTB.x) * 0.5;
    const float  SlopeTB       = (EdgesLRTB.w - EdgesLRTB.z) * 0.5;
    const float4 SlopeAdjusted = EdgesLRTB + float4(SlopeLR, -SlopeLR, SlopeTB, -SlopeTB);

    EdgesLRTB = min(abs(EdgesLRTB), abs(SlopeAdjusted));
    return saturate(1.25 - EdgesLRTB / (CenterZ * 0.011));
}

// Two bits per edge, so the denoiser reads four gradient steps instead of a hard in or out.
float XeGTAO_PackEdges(float4 EdgesLRTB)
{
    EdgesLRTB = round(saturate(EdgesLRTB) * 2.9);
    return dot(EdgesLRTB, float4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

float4 XeGTAO_UnpackEdges(float PackedValue)
{
    const uint Packed = (uint)(PackedValue * 255.5);

    float4 EdgesLRTB;
    EdgesLRTB.x = float((Packed >> 6) & 0x03) / 3.0;
    EdgesLRTB.y = float((Packed >> 4) & 0x03) / 3.0;
    EdgesLRTB.z = float((Packed >> 2) & 0x03) / 3.0;
    EdgesLRTB.w = float((Packed >> 0) & 0x03) / 3.0;
    return saturate(EdgesLRTB);
}

// The quadrant weights keep every cross product on one surface, so no normal spans a silhouette.
float3 XeGTAO_CalculateNormal(float4 EdgesLRTB, float3 Center, float3 PosL, float3 PosR, float3 PosT, float3 PosB)
{
    const float4 Accepted = saturate(float4(EdgesLRTB.x * EdgesLRTB.z,
                                            EdgesLRTB.z * EdgesLRTB.y,
                                            EdgesLRTB.y * EdgesLRTB.w,
                                            EdgesLRTB.w * EdgesLRTB.x) + 0.01);

    PosL = normalize(PosL - Center);
    PosR = normalize(PosR - Center);
    PosT = normalize(PosT - Center);
    PosB = normalize(PosB - Center);

    const float3 N = Accepted.x * cross(PosL, PosT)
                   + Accepted.y * cross(PosT, PosR)
                   + Accepted.z * cross(PosR, PosB)
                   + Accepted.w * cross(PosB, PosL);
    return normalize(N);
}

// http://h14s.p5r.org/2012/09/0x5f3759df.html
float XeGTAO_FastSqrt(float X)
{
    return asfloat(0x1fbd1df5 + (asint(X) >> 1));
}

// https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture
float XeGTAO_FastACos(float InX)
{
    const float X   = abs(InX);
    float       Res = -0.156583 * X + XE_GTAO_PI_HALF;
    Res *= XeGTAO_FastSqrt(1.0 - X);
    return (InX >= 0.0) ? Res : XE_GTAO_PI - Res;
}

#define XE_HILBERT_LEVEL 6u
#define XE_HILBERT_WIDTH (1u << XE_HILBERT_LEVEL)

uint XeGTAO_HilbertIndex(uint PosX, uint PosY)
{
    uint Index = 0u;
    for (uint CurLevel = XE_HILBERT_WIDTH / 2u; CurLevel > 0u; CurLevel /= 2u)
    {
        const uint RegionX = ((PosX & CurLevel) > 0u) ? 1u : 0u;
        const uint RegionY = ((PosY & CurLevel) > 0u) ? 1u : 0u;

        Index += CurLevel * CurLevel * ((3u * RegionX) ^ RegionY);

        if (RegionY == 0u)
        {
            if (RegionX == 1u)
            {
                PosX = (XE_HILBERT_WIDTH - 1u) - PosX;
                PosY = (XE_HILBERT_WIDTH - 1u) - PosY;
            }

            const uint Temp = PosX;
            PosX = PosY;
            PosY = Temp;
        }
    }
    return Index;
}

// Hilbert-curve-driven R2 sequence; 288 is the reference's empirical temporal stride.
float2 XeGTAO_SpatioTemporalNoise(uint2 PixCoord, uint TemporalIndex)
{
    uint Index = XeGTAO_HilbertIndex(PixCoord.x % 64u, PixCoord.y % 64u);
    Index += 288u * (TemporalIndex % 64u);
    return frac(0.5 + float(Index) * float2(0.75487766624669276005, 0.5698402909980532659114));
}

// Weighted max, not average, or a mip texel drifts behind the silhouette and thin objects halo.
float XeGTAO_DepthMIPFilter(float Depth0, float Depth1, float Depth2, float Depth3,
                            float EffectRadius, float EffectFalloffRange)
{
    const float MaxDepth = max(max(Depth0, Depth1), max(Depth2, Depth3));

    const float DepthRangeScaleFactor = 0.75;
    const float Radius       = DepthRangeScaleFactor * EffectRadius;
    const float FalloffRange = max(EffectFalloffRange * Radius, 1e-6);
    const float FalloffFrom  = Radius * (1.0 - EffectFalloffRange);
    const float FalloffMul   = -1.0 / FalloffRange;
    const float FalloffAdd   = FalloffFrom / FalloffRange + 1.0;

    const float W0 = saturate((MaxDepth - Depth0) * FalloffMul + FalloffAdd);
    const float W1 = saturate((MaxDepth - Depth1) * FalloffMul + FalloffAdd);
    const float W2 = saturate((MaxDepth - Depth2) * FalloffMul + FalloffAdd);
    const float W3 = saturate((MaxDepth - Depth3) * FalloffMul + FalloffAdd);

    const float WSum = W0 + W1 + W2 + W3;
    return (W0 * Depth0 + W1 * Depth1 + W2 * Depth2 + W3 * Depth3) / max(WSum, 1e-6);
}
