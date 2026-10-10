#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/XeGTAO.slang"

// Linearizes scene depth into the 5-mip viewspace pyramid the main pass steps through by sample radius.

struct FPushConstants
{
    uint2        ViewportSize;
    FTexture2D   SrcDepth;
    FRWTexture2D Mip0;
    FRWTexture2D Mip1;
    FRWTexture2D Mip2;
    FRWTexture2D Mip3;
    FRWTexture2D Mip4;
    float        EffectRadius;
    float        EffectFalloffRange;
    uint         SrcScale;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

groupshared float gsScratchDepths[XE_GTAO_NUMTHREADS_X][XE_GTAO_NUMTHREADS_Y];

void StoreDepth(FRWTexture2D Mip, uint2 Coord, float Depth)
{
    Mip[Coord] = float4(Depth, 0.0, 0.0, 0.0);
}

float FilterQuad(float D0, float D1, float D2, float D3)
{
    return XeGTAO_DepthMIPFilter(D0, D1, D2, D3, Pass().EffectRadius, Pass().EffectFalloffRange);
}

// One 8x8 group covers a 16x16 pixel tile, which is exactly one texel of mip 4.
[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
    const FGTAOView V = MakeGTAOView(Pass().ViewportSize);

    const uint2 BaseCoord = DTid.xy;
    const uint2 PixCoord  = BaseCoord * 2u;

    float4 Depths4;
    if (Pass().SrcScale == 1u)
    {
        // Gathering on the texel corner with a (1,1) offset lands the quad exactly on PixCoord..PixCoord+1.
        Depths4 = Pass().SrcDepth.GatherRed(
            Samplers::PointClamp, float2(PixCoord) * V.ViewportPixelSize, int2(1, 1));
    }
    else
    {
        // Each reduced texel keeps the top-left source depth of its block, which GTAOUpsample compares against.
        FTexture2D Src = Pass().SrcDepth;
        const uint S = Pass().SrcScale;
        Depths4.w = Src.Load(int3((PixCoord + uint2(0, 0)) * S, 0)).r;
        Depths4.z = Src.Load(int3((PixCoord + uint2(1, 0)) * S, 0)).r;
        Depths4.x = Src.Load(int3((PixCoord + uint2(0, 1)) * S, 0)).r;
        Depths4.y = Src.Load(int3((PixCoord + uint2(1, 1)) * S, 0)).r;
    }

    const float Depth0 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.w));
    const float Depth1 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.z));
    const float Depth2 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.x));
    const float Depth3 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.y));

    StoreDepth(Pass().Mip0, PixCoord + uint2(0, 0), Depth0);
    StoreDepth(Pass().Mip0, PixCoord + uint2(1, 0), Depth1);
    StoreDepth(Pass().Mip0, PixCoord + uint2(0, 1), Depth2);
    StoreDepth(Pass().Mip0, PixCoord + uint2(1, 1), Depth3);

    const float Mip1 = FilterQuad(Depth0, Depth1, Depth2, Depth3);
    StoreDepth(Pass().Mip1, BaseCoord, Mip1);
    gsScratchDepths[GTid.x][GTid.y] = Mip1;

    GroupMemoryBarrierWithGroupSync();

    [branch]
    if (all((GTid.xy % 2u) == 0u))
    {
        const float Mip2 = FilterQuad(gsScratchDepths[GTid.x + 0][GTid.y + 0],
                                      gsScratchDepths[GTid.x + 1][GTid.y + 0],
                                      gsScratchDepths[GTid.x + 0][GTid.y + 1],
                                      gsScratchDepths[GTid.x + 1][GTid.y + 1]);
        StoreDepth(Pass().Mip2, BaseCoord / 2u, Mip2);
        gsScratchDepths[GTid.x][GTid.y] = Mip2;
    }

    GroupMemoryBarrierWithGroupSync();

    [branch]
    if (all((GTid.xy % 4u) == 0u))
    {
        const float Mip3 = FilterQuad(gsScratchDepths[GTid.x + 0][GTid.y + 0],
                                      gsScratchDepths[GTid.x + 2][GTid.y + 0],
                                      gsScratchDepths[GTid.x + 0][GTid.y + 2],
                                      gsScratchDepths[GTid.x + 2][GTid.y + 2]);
        StoreDepth(Pass().Mip3, BaseCoord / 4u, Mip3);
        gsScratchDepths[GTid.x][GTid.y] = Mip3;
    }

    GroupMemoryBarrierWithGroupSync();

    [branch]
    if (all((GTid.xy % 8u) == 0u))
    {
        const float Mip4 = FilterQuad(gsScratchDepths[GTid.x + 0][GTid.y + 0],
                                      gsScratchDepths[GTid.x + 4][GTid.y + 0],
                                      gsScratchDepths[GTid.x + 0][GTid.y + 4],
                                      gsScratchDepths[GTid.x + 4][GTid.y + 4]);
        StoreDepth(Pass().Mip4, BaseCoord / 8u, Mip4);
    }
}
