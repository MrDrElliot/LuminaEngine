#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"

// Its own pixel pass so lighting stays free of the switch and the penumbra readout can use ddx/ddy.

struct FSceneDebugViewArgs
{
    FGBufferTargets GBuffer;
    FTexture2DUInt  VisBuffer;
    FTexture2D      Depth;
    uint            DrawListCount;
    uint            _Pad0;
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

    const uint Raw = Pass().VisBuffer.Load(Texel);
    if (Raw == 0u)
    {
        discard;
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const FGBufferTargets Targets = Pass().GBuffer;

    const FGBufferData G = LoadGBuffer(Targets, Texel);

    const float  RawDepth      = Pass().Depth.Load(Texel).r;
    const float3 ViewPosition  = ReconstructViewPos(In.UV, RawDepth, GetInverseCameraProjection());
    const float3 WorldPosition = mul(GetInverseCameraView(), float4(ViewPosition, 1.0)).xyz;

    const FVisTriangle T = ResolveVisTriangle(Raw, Pass().DrawListCount);

    // Triangle identity only exists in the VisBuffer, so it is resolved here rather than in ApplyDebugView.
    const uint DebugMode = Scene().CullData.DebugMode;
    if (DebugMode == DEBUG_MODE_TRIANGLE_ID)
    {
        return float4(MeshletDebugColor(T.Ref.GlobalMeshletIdx * 131u + T.TriIndex), 1.0);
    }

    // Reads its 2x2 neighbors, so it belongs here with the VisBuffer rather than in ApplyDebugView.
    if (DebugMode == DEBUG_MODE_QUAD_EFFICIENCY)
    {
        return float4(QuadEfficiencyColor(QuadLaneEfficiency(Pass().VisBuffer, Texel)), 1.0);
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
        // Free here since the reconstruction already produced the barycentrics and their derivatives.
        Info.EdgeDistancePixels = TriangleEdgeDistancePixels(S.Bary);
    }

    // Shaded as the base because modes ApplyDebugView does not own, like the OIT views, must leave opaque geometry lit.
    const float3 Base = ShadeGBuffer(G, WorldPosition, ViewPosition, In.Position.xy).Color;

    return float4(ApplyDebugView(DebugMode, Base, G, Info, WorldPosition, ViewPosition, In.Position.xy), 1.0);
}
