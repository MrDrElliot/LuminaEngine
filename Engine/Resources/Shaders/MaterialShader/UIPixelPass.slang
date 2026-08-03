#include "Includes/UIMaterialGlobals.slang"

// UI material template; fullscreen pass into an offscreen brush texture RmlUi samples. Output is
// premultiplied (Emissive * Opacity) to match RmlUi's premultiplied-alpha blend (One, OneMinusSrcAlpha).

struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    uint   MaterialIndex = UIArgs().MaterialIndex;
    float2 UV0           = Input.UV;

    // Surface aliases. Zeroed because there is no rasterized geometry feeding
    // this pass; node-emit code may still reference them.
    float3 WorldPosition = float3(0.0);
    float3 WorldNormal   = float3(0.0, 0.0, 1.0);
    float3 ViewPosition  = float3(0.0);
    float4 VertexColor   = float4(1.0);
    uint   EntityID      = 0;
    float4 WorldTangent  = float4(1.0, 0.0, 0.0, 1.0);
    float2 UV0_DDX       = ddx(UV0);
    float2 UV0_DDY       = ddy(UV0);

    $MATERIAL_INPUTS

    return float4(Material.Emissive * Material.Opacity, Material.Opacity);
}
