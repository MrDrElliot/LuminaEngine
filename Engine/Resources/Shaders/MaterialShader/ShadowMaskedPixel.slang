#include "Includes/SurfaceShading.slang"

// Depth-only shadow pixel shader for MASKED materials. Runs the material's pixel graph far enough to get
// Opacity and discards the cut-out texels, so foliage stops casting the shadow of its whole quad.
// Writes no target, exactly like the opaque shadow path, and takes the plain cutoff rather than the hashed
// one: a shadow map is already filtered by PCF, and stochastic coverage there reads as noise in the shade.
//
// The material token appears exactly once below, inside the keep test. Never name it above its real site.

bool ShadowMaskedKeep(FMaskedVertexOutput Input)
{
    uint EntityID           = MaskedEntityID(Input);
    uint MaterialIndex      = Input.MaterialIndex;
    bool bReceiveShadow     = (MaskedInstanceFlags(Input) & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal      = MaskedWorldNormal(Input);
    float3 ViewPosition     = MaskedShadowViewPosition(Input);
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

    return Material.Opacity >= GetMaterialOpacityClip(MaterialIndex);
}

[shader("fragment")]
void FragmentMain(FMaskedVertexOutput Input)
{
    if (!ShadowMaskedKeep(Input))
    {
        discard;
    }
}
