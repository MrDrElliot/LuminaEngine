#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Averages the contact traces over each pixel's 4x4 box, which holds every ordered jitter once, and folds them into the far mask.

// Mirror of Lumina::FSunContactShadowArgs, where FarMask is the mask the trace pass wrote.
struct FPushConstants
{
    uint2          ScreenSize;
    FTexture2D     Depth;
    FTexture2D     GBufferB;
    float3         ToSun;
    float          Length;
    FTexture2DUInt SlotImage;
    FTexture2D     FarMask;
    FRWTexture2D   Output;
    uint           Steps;
    float          MinPixels;
    uint           _Pad0;
    uint           _Pad1;
    uint           _Pad2;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// The 4x4 box reaches two texels before each pixel and one after.
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

        // Reversed Z clears to zero, so the sky stays out of every box.
        const float Raw = all(P >= 0) && all(P < int2(Pass().ScreenSize)) ? Pass().Depth.Load(int3(P, 0)).r : 0.0;
        if (Raw > 0.0)
        {
            Contact = Pass().FarMask.Load(int3(P, 0)).g;
            Depth   = abs(ReconstructViewPos((float2(P) + 0.5) / Size, Raw, GetInverseCameraProjection()).z);
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

    const float2 Stored     = Pass().FarMask.Load(int3(Pixel, 0)).rg;
    float        Visibility = Stored.x;

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

    // Green is written back unchanged, since neighboring groups still read it for their boxes.
    Pass().Output[Pixel] = float4(Visibility, Stored.y, 0.0, 0.0);
}
