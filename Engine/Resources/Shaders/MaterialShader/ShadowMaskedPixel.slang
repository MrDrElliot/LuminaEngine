#include "Includes/SurfaceShading.slang"

// Depth-only shadow pixel shader for MASKED materials. Runs the material's pixel graph far enough to get
// Opacity and discards the cut-out texels, so foliage stops casting the shadow of its whole quad.
// Writes no target, exactly like the opaque shadow path, and takes the plain cutoff rather than the hashed
// one: a shadow map is already filtered by PCF, and stochastic coverage there reads as noise in the shade.
//
// The material token appears exactly once below, inside the keep test. Never name it above its real site.

bool ShadowMaskedKeep(FBaseVertexOutput Input)
{
    uint EntityID           = Input.EntityID;
    uint MaterialIndex      = Input.MaterialIndex;
    bool bReceiveShadow     = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal      = normalize(Input.NormalWS.xyz);
    float3 ViewPosition     = Input.FragPos.xyz;
    float3 WorldPosition    = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
    float2 UV0              = Input.UV;
    float2 UV1              = Input.UV1;
    float4 VertexColor      = Input.FragColor;
    float4 WorldTangent     = Input.TangentWS;
    float2 UV1_DDX          = ddx(UV1);
    float2 UV1_DDY          = ddy(UV1);
    float2 UV0_DDX          = ddx(UV0);
    float2 UV0_DDY          = ddy(UV0);
    float3 WorldPosition_DDX = ddx(WorldPosition);
    float3 WorldPosition_DDY = ddy(WorldPosition);
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    $MATERIAL_INPUTS

    return Material.Opacity >= Materials()[MaterialIndex].OpacityClipValue;
}

[shader("fragment")]
void FragmentMain(FBaseVertexOutput Input)
{
    if (!ShadowMaskedKeep(Input))
    {
        discard;
    }
}
