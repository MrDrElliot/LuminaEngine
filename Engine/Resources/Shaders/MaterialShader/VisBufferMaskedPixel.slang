#include "Includes/SurfaceShading.slang"

// The compiler substitutes the first material-inputs token, so never name it in a comment above its use.

// One texel in sixteen asks, which is plenty of coverage and keeps the atomics off every discarded leaf.
static const uint kMaskedFeedbackStrideMask = 3u;

// Returns true when the texel should be kept, its opacity at or above the material's cutoff.
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

    // Anchored in world space so the dithered edge holds still as the camera moves.
    const bool bKeep = MaskedAlphaKeep(WorldPosition, length(ViewPosition), Material.Opacity, GetMaterialOpacityClip(MaterialIndex));

    // A discarded texel never reaches shading, so a mask too blurry to pass its cutoff asks for sharper mips here or never would.
    const uint2 Pixel = uint2(Input.Position.xy);
    if (!bKeep && ((Pixel.x | Pixel.y) & kMaskedFeedbackStrideMask) == 0u)
    {
        RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
    }
    return bKeep;
}

[shader("fragment")]
uint FragmentMain(FMaskedVertexOutput Input, VIS_ID_INPUT) : SV_Target0
{
    if (!MaskedVisKeep(Input))
    {
        discard;
    }
    return VisID;
}
