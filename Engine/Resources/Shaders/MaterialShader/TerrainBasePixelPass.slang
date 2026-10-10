#include "Includes/TerrainData.slang"
#include "Includes/SurfaceShading.slang"

// The terrain pipeline specializes SPEC_ENABLE_GTAO = 0, so ShadeSurface's AO block is stripped.

struct PSInput
{
    float4 FragColor                : COLOR;
    float4 NormalVS                 : NORMAL_VS;
    float4 NormalWS                 : NORMAL_WS;
    float4 TangentWS                : TANGENT_WS;
    float4 FragPos                  : FRAG_POS;
    // Mirrors VSOutput in TerrainBaseVertexPass.slang; the two structs match by semantic, not by name.
    float3 WorldPosNoWPO            : WORLD_POS_NO_WPO;
    float2 UV                       : TEXCOORD0;
    float2 HeightUV                 : HEIGHT_UV;

    nointerpolation uint EntityID      : ENTITY_ID;
    nointerpolation uint MaterialIndex : MATERIAL_INDEX;
    nointerpolation uint MeshletIndex  : MESHLET_INDEX;

    float4 Position                 : SV_Position;
};

struct PSOutput
{
    float4 Color    : SV_Target0;
    uint   Picker   : SV_Target1;
};

// The masked variant drops forced early-Z because its discard must run before the depth write. Mirrors BasePixelPass.slang.
#ifndef MASKED
[earlydepthstencil]
#endif
[shader("fragment")]
PSOutput FragmentMain(PSInput Input)
{
    // Material graph locals, with UV0 aliased to world XZ.
    uint   EntityID      = Input.EntityID;
    uint   MaterialIndex = Input.MaterialIndex;
    float3 WorldNormal   = normalize(Input.NormalWS.xyz);
    float3 ViewPosition  = Input.FragPos.xyz;
    float3 WorldPosition = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
    float3 WorldPositionExcludingOffsets = Input.WorldPosNoWPO;
    float2 UV0           = Input.UV;
    float2 HeightUV      = Input.HeightUV;
    float4 VertexColor   = Input.FragColor;
    float4 WorldTangent  = Input.TangentWS;
    // Terrain has no authored second UV set, so set 1 aliases set 0 rather than reading zero.
    float2 UV1           = UV0;
    float2 UV1_DDX       = ddx(UV1);
    float2 UV1_DDY       = ddy(UV1);
    float2 UV0_DDX       = ddx(UV0);
    float2 UV0_DDY       = ddy(UV0);
    float3 WorldPosition_DDX = ddx(WorldPosition);
    float3 WorldPosition_DDY = ddy(WorldPosition);
    float3 WorldPositionExcludingOffsets_DDX = ddx(WorldPositionExcludingOffsets);
    float3 WorldPositionExcludingOffsets_DDY = ddy(WorldPositionExcludingOffsets);
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    // A real pixel shader, so these are the quad's implicit derivatives.
    RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
    $MATERIAL_INPUTS

    // Terrain has no per-instance data, so the instance fields carry constants.
    FBaseVertexOutput Surface;
    Surface.FragColor     = Input.FragColor;
    Surface.NormalVS      = Input.NormalVS;
    Surface.NormalWS      = Input.NormalWS;
    Surface.TangentWS     = Input.TangentWS;
    Surface.FragPos       = Input.FragPos;
    Surface.WorldPosNoWPO = Input.WorldPosNoWPO;
    Surface.UV            = Input.UV;
    Surface.InstanceIndex = 0;
    Surface.EntityID      = Input.EntityID;
    Surface.MaterialIndex = Input.MaterialIndex;
    Surface.InstanceFlags = (uint)EInstanceFlags::ReceiveShadow;
    Surface.MeshletIndex  = Input.MeshletIndex;
    Surface.Position      = Input.Position;

    FShadedSurface Shaded = ShadeSurface(Surface, Material);

    PSOutput Output;
    Output.Color  = float4(Shaded.Color, Shaded.Alpha);
    Output.Picker = EntityID;
    return Output;
}
