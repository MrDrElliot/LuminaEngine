// Compute lane, so nothing here may use ddx/ddy; the debug-view switch lives in SceneDebugViewPixel.slang instead.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHIStorage.slang"

struct FDeferredLightingArgs
{
    uint  GBufferAIndex;
    uint  GBufferBIndex;
    uint  GBufferCIndex;
    uint  GBufferDIndex;
    uint  DepthIndex;
    uint  HDRUAV;
    uint  ScreenW;
    uint  ScreenH;
    uint  SlotImageIndex;  // slot + 1 per pixel from the classify count, 0 where nothing deferred shades it
    uint  _Pad0;
    uint  _Pad1;
    uint  _Pad2;
};
FDeferredLightingArgs* Pass() { return GetPassArgs<FDeferredLightingArgs>(); }

// Screen tiles, each pixel lit only when classification gave it a GBuffer.
[shader("compute")]
[numthreads(MATERIAL_CLASSIFY_TILE, MATERIAL_CLASSIFY_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 Texel = DTid.xy;
    if (Texel.x >= Pass().ScreenW || Texel.y >= Pass().ScreenH)
    {
        return;
    }

    if (LoadTexture2D(Pass().SlotImageIndex, int2(Texel)).r < 0.5)
    {
        return;
    }

    FGBufferTargets Targets;
    Targets.A = Pass().GBufferAIndex;
    Targets.B = Pass().GBufferBIndex;
    Targets.C = Pass().GBufferCIndex;
    Targets.D = Pass().GBufferDIndex;

    const FGBufferData G = LoadGBuffer(Targets, int2(Texel));

    const float2 ScreenPos = float2(Texel) + 0.5;
    const float2 UV        = ScreenPos / float2(Pass().ScreenW, Pass().ScreenH);
    const float  RawDepth  = LoadTexture2D(Pass().DepthIndex, int2(Texel)).r;

    const float3 ViewPosition  = ReconstructViewPos(UV, RawDepth, GetInverseCameraProjection());
    const float3 WorldPosition = mul(GetInverseCameraView(), float4(ViewPosition, 1.0)).xyz;

    const FShadedSurface Shaded = ShadeGBuffer(G, WorldPosition, ViewPosition, ScreenPos);

    gRWTextures2D[Pass().HDRUAV][int2(Texel)] = float4(Shaded.Color, 1.0);
}
