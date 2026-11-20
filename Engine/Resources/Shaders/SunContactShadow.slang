#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/GBuffer.slang"
#include "Includes/ShadowSampling.slang"

// Short screen-space march toward the sun, folded into the sun mask so props stop floating where a shadow texel is coarser than the gap.

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

float TraceContact(int2 Pixel, float Raw, float3 ViewP)
{
    const float2 Size = float2(Pass().ScreenSize);
    if (ContactReachPixels(Pass().Length, ViewP.z, Size.y) < Pass().MinPixels)
    {
        return 1.0;
    }

    // Custom depth geometry leaves no GBuffer, so only deferred pixels carry the opt-out bit and a stored normal.
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
    return ContactShadow(ViewP, ViewN, ViewL, OrderedContactJitter(Pixel), Pass().Length, Pass().Steps);
}

// Each tile traces itself plus the 4x4 box's reach, two texels before and one after, then resolves the ordered jitter in the box.
static const uint kTile     = 16u;
static const int  kBoxLow   = 2;
static const uint kRegion   = kTile + 3u;
static const uint kCells    = kRegion * kRegion;

// Neighbors farther than this fraction of the center's depth are another surface and stay out of its box.
static const float kResolveDepthTolerance = 0.02;

groupshared float GContact[kCells];
groupshared float GDepth[kCells];

[shader("compute")]
[numthreads(kTile, kTile, 1)]
void ComputeMain(uint3 GroupId : SV_GroupID, uint3 GroupThread : SV_GroupThreadID)
{
    const int2   Origin = int2(GroupId.xy * kTile) - kBoxLow;
    const float2 Size   = float2(Pass().ScreenSize);

    for (uint Cell = GroupThread.y * kTile + GroupThread.x; Cell < kCells; Cell += kTile * kTile)
    {
        const int2 P = Origin + int2(Cell % kRegion, Cell / kRegion);
        float Contact = 1.0;
        float Depth   = 0.0;

        // Reversed Z clears to zero, so the sky stays lit and out of every box.
        const float Raw = all(P >= 0) && all(P < int2(Pass().ScreenSize)) ? Pass().Depth.Load(int3(P, 0)).r : 0.0;
        if (Raw > 0.0)
        {
            const float3 ViewP = ReconstructViewPos((float2(P) + 0.5) / Size, Raw, GetInverseCameraProjection());
            Contact = TraceContact(P, Raw, ViewP);
            Depth   = abs(ViewP.z);
        }
        GContact[Cell] = Contact;
        GDepth[Cell]   = Depth;
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 Pixel = int2(GroupId.xy * kTile + GroupThread.xy);
    if (any(Pixel >= int2(Pass().ScreenSize)))
    {
        return;
    }

    float Visibility = Pass().FarMask.IsValid() ? Pass().FarMask.Load(int3(Pixel, 0)).r : 1.0;

    const uint  Center      = (GroupThread.y + kBoxLow) * kRegion + GroupThread.x + kBoxLow;
    const float CenterDepth = GDepth[Center];
    if (CenterDepth > 0.0 && Visibility > 0.0)
    {
        const float Tolerance = CenterDepth * kResolveDepthTolerance;
        float Sum    = 0.0;
        float Weight = 0.0;
        [unroll]
        for (int Y = 0; Y < 4; ++Y)
        {
            [unroll]
            for (int X = 0; X < 4; ++X)
            {
                const uint  Cell = (GroupThread.y + Y) * kRegion + GroupThread.x + X;
                const float W    = GDepth[Cell] > 0.0 ? saturate(1.0 - abs(GDepth[Cell] - CenterDepth) / Tolerance) : 0.0;
                Sum    += GContact[Cell] * W;
                Weight += W;
            }
        }
        Visibility *= Sum / max(Weight, 1e-4);
    }

    Pass().Output[Pixel] = float4(Visibility, 0.0, 0.0, 0.0);
}
