// Compute lane: no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample. The
// directional shadow comes from ShadowMaskPixel.slang's screen-space mask instead of the inline cascade
// PCSS, and the debug-view switch is not compiled in at all (SceneDebugViewPixel.slang owns it).
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHIStorage.slang"

// Deferred lighting -- the final step of the visibility-buffer pipeline.
//
// ONE indirect dispatch over the whole compacted pixel list, which is every pixel the material passes
// wrote a GBuffer for, each exactly once. Nothing else needs checking: background, terrain and any material
// without a deferred shader were never classified, so they are simply not in the list and their HDR pixels
// are left as the environment pass wrote them.
//
// Lighting cost is therefore independent of triangle density -- the point of the whole design. It is also
// why this shader exists at all rather than lighting inside the material pass: there is one light loop, one
// shadow lookup and one IBL evaluation per pixel no matter how many materials are on screen.
//
// World position is reconstructed from the depth buffer rather than stored: the VisBuffer geometry pass
// wrote VisID and depth together, so the depth at this texel belongs to exactly the triangle whose material
// filled this GBuffer texel.

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
    uint* PixelList;
    uint* Total;         // uint[1], written by VisBufferMaterialPrefixSum
};
FDeferredLightingArgs* Pass() { return GetPassArgs<FDeferredLightingArgs>(); }

[shader("compute")]
[numthreads(MATERIAL_PIXEL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    // Grid folded across Y by the prefix sum, for the same reason the material dispatches are.
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

    const float2 ScreenPos = float2(Texel) + 0.5;
    const float2 UV        = ScreenPos / float2(Pass().ScreenW, Pass().ScreenH);
    const float  RawDepth  = LoadTexture2D(Pass().DepthIndex, int2(Texel)).r;

    const float3 ViewPosition  = ReconstructViewPos(UV, RawDepth, GetInverseCameraProjection());
    const float3 WorldPosition = mul(GetInverseCameraView(), float4(ViewPosition, 1.0)).xyz;

    const FShadedSurface Shaded = ShadeGBuffer(G, WorldPosition, ViewPosition, ScreenPos);

    // Opaque: alpha is 1 by definition. Masked cut-outs were clipped in the geometry stage and never
    // reached the VisBuffer, so nothing in this list is partially covered.
    gRWTextures2D[Pass().HDRUAV][int2(Texel)] = float4(Shaded.Color, 1.0);
}
