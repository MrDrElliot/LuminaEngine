#include "Includes/SurfaceShading.slang"

// Masked VisBuffer pixel shader. Runs the material's pixel graph far enough to evaluate Material.Opacity and
// `discard`s cut-out texels BEFORE they write VisID/depth, so masked geometry no longer stamps full-coverage
// depth over whatever is behind its holes. The deferred material pass keeps its own ShadeSurface masked clip
// for silhouette edges; this one fixes coverage/occlusion.
//
// Two entry points share one opacity evaluation:
// The material-graph token appears exactly once (inside MaskedVisKeep): the compiler substitutes the FIRST
// occurrence, so it must never appear in a comment above it -- that would inject the codegen at file scope.

// Runs the pixel graph and returns true when the texel should be KEPT (opacity >= the material's cutoff).
// `discard` is legal from a helper called by a pixel shader. Reuses the exact local contract of BasePixelPass.
bool MaskedVisKeep(FMaskedVertexOutput Input)
{
    uint EntityID           = MaskedEntityID(Input);
    uint MaterialIndex      = Input.MaterialIndex;
    bool bReceiveShadow     = (MaskedInstanceFlags(Input) & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal      = MaskedWorldNormal(Input);
    float3 ViewPosition     = MaskedCameraViewPosition(Input);
    float3 WorldPosition    = mul(GetInverseCameraView(), float4(ViewPosition, 1.0)).xyz;
    float3 WorldPositionExcludingOffsets = MaskedWorldPosNoWPO(Input, WorldPosition);
    float2 UV0              = Input.UV;
    float2 UV1              = MaskedUV1(Input);
    float4 VertexColor      = MaskedVertexColor(Input);
    float4 WorldTangent     = MaskedWorldTangent(Input);
    float2 UV1_DDX          = ddx(UV1);
    float2 UV1_DDY          = ddy(UV1);
    float2 UV0_DDX          = ddx(UV0);
    float2 UV0_DDY          = ddy(UV0);
    float3 WorldPosition_DDX = ddx(WorldPosition);
    float3 WorldPosition_DDY = ddy(WorldPosition);
    float3 WorldPositionExcludingOffsets_DDX = ddx(WorldPositionExcludingOffsets);
    float3 WorldPositionExcludingOffsets_DDY = ddy(WorldPositionExcludingOffsets);
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    $MATERIAL_INPUTS

    // Anchored in world space, so the verdict for a surface point is the same every frame and the
    // dithered edge holds still instead of crawling as the camera moves.
    const float  ClipCutoff = HashedAlphaThreshold(WorldPosition, length(ViewPosition),
                                                   GetMaterialOpacityClip(MaterialIndex));
    return Material.Opacity >= ClipCutoff;
}


[shader("fragment")]
uint FragmentMain(FMaskedVertexOutput Input, uint VisID : SV_PrimitiveID) : SV_Target0
{
    if (!MaskedVisKeep(Input))
    {
        discard;
    }
    return VisID;
}

