#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/GBuffer.slang"
#include "Includes/ShadowSampling.slang"

// Short screen-space march toward the sun, folded into the sun mask so props stop floating where a cascade texel is coarser than the gap.

// Mirror of Lumina::FSunContactShadowArgs.
struct FPushConstants
{
    uint2          ScreenSize;
    FTexture2D     Depth;
    FTexture2D     GBufferB;
    float3         ToSun;
    float          Length;
    FTexture2DUInt SlotImage;  // slot + 1 per pixel from the classify count, 0 where nothing deferred shades it
    FTexture2D     FarMask;    // what the far trace left, invalid when it did not run
    FRWTexture2D   Output;
    uint           Steps;
    float          MinPixels;  // reach on screen below which a pixel is too far for the march to find anything
    uint           _Pad0;
    uint           _Pad1;
    uint           _Pad2;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float TraceContact(int2 Pixel, float Raw)
{
    const float2 Size  = float2(Pass().ScreenSize);
    const float3 ViewP = ReconstructViewPos((float2(Pixel) + 0.5) / Size, Raw, GetInverseCameraProjection());
    if (ContactReachPixels(Pass().Length, ViewP.z, Size.y) < Pass().MinPixels)
    {
        return 1.0;
    }

    // Terrain shades forward and leaves no GBuffer, so only deferred pixels carry the opt-out bit and a stored normal.
    float3 ViewN;
    if (Pass().SlotImage.Load(Pixel) != 0u)
    {
        const float4 B = Pass().GBufferB.Load(int3(Pixel, 0));
        if (B.a >= 0.5)
        {
            return 1.0;
        }
        ViewN = NormalWorldToView(OctDecode01(B.rg));
    }
    else
    {
        ViewN = GeometricViewNormal(Pass().Depth, Pixel, Raw, ViewP, Size);
    }

    const float3 ViewL = NormalWorldToView(Pass().ToSun);
    if (dot(ViewN, ViewL) <= 0.0)
    {
        return 1.0;
    }
    return ContactShadow(ViewP, ViewN, ViewL, InterleavedGradientNoise(float2(Pixel) + 0.5), Pass().Length, Pass().Steps);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const int2 Pixel = int2(DTid.xy);
    if (any(DTid.xy >= Pass().ScreenSize))
    {
        return;
    }

    float Visibility = Pass().FarMask.IsValid() ? Pass().FarMask.Load(int3(Pixel, 0)).r : 1.0;

    // Reversed Z clears to zero, so the sky stays lit.
    const float Raw = Pass().Depth.Load(int3(Pixel, 0)).r;
    if (Raw > 0.0 && Visibility > 0.0)
    {
        Visibility *= TraceContact(Pixel, Raw);
    }

    Pass().Output[Pixel] = float4(Visibility, 0.0, 0.0, 0.0);
}
