#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"

// Editor view modes for the deferred lane, as a full-screen overlay run only when a debug mode is active.
//
// It lives in its own pass, and its own PIXEL shader, for two reasons. The production lighting shader stays
// free of the whole switch -- no branch, no register pressure from paths a shipping build never takes. And
// the penumbra readout differentiates shadow-space depth with ddx/ddy, which the compute lighting pass
// cannot do at all.
//
// Only pixels the deferred lane owns are touched. Terrain and translucency shade forward through
// ShadeSurface and apply the identical switch inline, so their pixels are already correct and are
// discarded here.

struct FSceneDebugViewArgs
{
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint GBufferDIndex;
    uint VisBufferIndex;
    uint DepthIndex;
    uint DrawListCount;
    uint _Pad0;
};
FSceneDebugViewArgs* Pass() { return GetPassArgs<FSceneDebugViewArgs>(); }

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(FragmentInput In) : SV_Target0
{
    const int2 Texel = int2(In.Position.xy);

    const uint Raw = LoadTexture2DUInt(Pass().VisBufferIndex, Texel);
    if (Raw == 0u)
    {
        discard;
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    FGBufferTargets Targets;
    Targets.A = Pass().GBufferAIndex;
    Targets.B = Pass().GBufferBIndex;
    Targets.C = Pass().GBufferCIndex;
    Targets.D = Pass().GBufferDIndex;

    const FGBufferData G = LoadGBuffer(Targets, Texel);

    const float  RawDepth      = LoadTexture2D(Pass().DepthIndex, Texel).r;
    const float3 ViewPosition  = ReconstructViewPos(In.UV, RawDepth, GetInverseCameraProjection());
    const float3 WorldPosition = mul(GetInverseCameraView(), float4(ViewPosition, 1.0)).xyz;

    const FVisTriangle T = ResolveVisTriangle(Raw, Pass().DrawListCount);

    // Triangle identity only exists in the VisBuffer, so it is resolved here rather than in ApplyDebugView.
    const uint DebugMode = Scene().CullData.DebugMode;
    if (DebugMode == DEBUG_MODE_TRIANGLE_ID)
    {
        return float4(MeshletDebugColor(T.Ref.GlobalMeshletIdx * 131u + T.TriIndex), 1.0);
    }

    FDebugSurfaceInfo Info;
    Info.UV                 = float2(0.0, 0.0);
    Info.GeometricNormalWS  = G.WorldNormal;
    Info.MeshletIndex       = T.Ref.GlobalMeshletIdx;
    Info.MaterialIndex      = 0u;
    Info.EdgeDistancePixels = kNoEdgeDistance;

    if (T.bValid)
    {
        const float2      ScreenSize = float2(Scene().ScreenSize.xy);
        const FVisSurface S          = ReconstructVisSurfaceNoWPO(T, uint2(Texel), ScreenSize);

        Info.UV                = S.UV;
        Info.GeometricNormalWS = S.NormalWS;
        Info.MaterialIndex     = T.Ref.Inst.MaterialIndex;
        // Free here: the reconstruction already produced the barycentrics and their derivatives.
        Info.EdgeDistancePixels = TriangleEdgeDistancePixels(S.Bary);
    }

    // The lit color is the BASE the switch starts from, not an optimization for the one view that tints
    // it. ApplyDebugView leaves Color untouched for any mode it does not own -- the OIT views, which
    // OITResolve draws over translucent pixels later -- and those modes must leave opaque geometry lit,
    // not black. Re-shading rather than reading HDR back keeps this pass a plain overwrite of its own
    // render target; the cost is irrelevant in a mode that only exists in the editor.
    const float3 Base = ShadeGBuffer(G, WorldPosition, ViewPosition, In.Position.xy).Color;

    return float4(ApplyDebugView(DebugMode, Base, G, Info, WorldPosition, ViewPosition, In.Position.xy), 1.0);
}
