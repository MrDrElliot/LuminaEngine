#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/XeGTAO.slang"

// Linearizes scene depth into the 5-mip viewspace pyramid the main pass steps through by sample radius.

struct FPushConstants
{
    uint2 ViewportSize;
    uint  SrcDepthIndex;
    uint  MipUAV0;
    uint  MipUAV1;
    uint  MipUAV2;
    uint  MipUAV3;
    uint  MipUAV4;
    float EffectRadius;
    float EffectFalloffRange;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

groupshared float gScratchDepths[XE_GTAO_NUMTHREADS_X][XE_GTAO_NUMTHREADS_Y];

void StoreDepth(uint MipUAV, uint2 Coord, float Depth)
{
    gRWTextures2D[MipUAV][Coord] = float4(Depth, 0.0, 0.0, 0.0);
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

    // Gathering on the texel corner with a (1,1) offset lands the quad exactly on PixCoord..PixCoord+1.
    const float4 Depths4 = gTextures2D[Pass().SrcDepthIndex].GatherRed(
        gSamplers[SAMPLER_POINT_CLAMP], float2(PixCoord) * V.ViewportPixelSize, int2(1, 1));

    const float Depth0 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.w));
    const float Depth1 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.z));
    const float Depth2 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.x));
    const float Depth3 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(V, Depths4.y));

    StoreDepth(Pass().MipUAV0, PixCoord + uint2(0, 0), Depth0);
    StoreDepth(Pass().MipUAV0, PixCoord + uint2(1, 0), Depth1);
    StoreDepth(Pass().MipUAV0, PixCoord + uint2(0, 1), Depth2);
    StoreDepth(Pass().MipUAV0, PixCoord + uint2(1, 1), Depth3);

    const float Mip1 = FilterQuad(Depth0, Depth1, Depth2, Depth3);
    StoreDepth(Pass().MipUAV1, BaseCoord, Mip1);
    gScratchDepths[GTid.x][GTid.y] = Mip1;

    GroupMemoryBarrierWithGroupSync();

    [branch]
    if (all((GTid.xy % 2u) == 0u))
    {
        const float Mip2 = FilterQuad(gScratchDepths[GTid.x + 0][GTid.y + 0],
                                      gScratchDepths[GTid.x + 1][GTid.y + 0],
                                      gScratchDepths[GTid.x + 0][GTid.y + 1],
                                      gScratchDepths[GTid.x + 1][GTid.y + 1]);
        StoreDepth(Pass().MipUAV2, BaseCoord / 2u, Mip2);
        gScratchDepths[GTid.x][GTid.y] = Mip2;
    }

    GroupMemoryBarrierWithGroupSync();

    [branch]
    if (all((GTid.xy % 4u) == 0u))
    {
        const float Mip3 = FilterQuad(gScratchDepths[GTid.x + 0][GTid.y + 0],
                                      gScratchDepths[GTid.x + 2][GTid.y + 0],
                                      gScratchDepths[GTid.x + 0][GTid.y + 2],
                                      gScratchDepths[GTid.x + 2][GTid.y + 2]);
        StoreDepth(Pass().MipUAV3, BaseCoord / 4u, Mip3);
        gScratchDepths[GTid.x][GTid.y] = Mip3;
    }

    GroupMemoryBarrierWithGroupSync();

    [branch]
    if (all((GTid.xy % 8u) == 0u))
    {
        const float Mip4 = FilterQuad(gScratchDepths[GTid.x + 0][GTid.y + 0],
                                      gScratchDepths[GTid.x + 4][GTid.y + 0],
                                      gScratchDepths[GTid.x + 0][GTid.y + 4],
                                      gScratchDepths[GTid.x + 4][GTid.y + 4]);
        StoreDepth(Pass().MipUAV4, BaseCoord / 8u, Mip4);
    }
}
