#define MATERIAL_NO_MIP_BIAS 1
#include "Includes/SceneGlobals.slang"

// The compiler substitutes the first occurrence of the material-graph token, so never spell it in a comment above its use.
struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// Must match FLightFunctionArgs in SceneRenderer_Lighting.cpp.
struct FLightFunctionArgs
{
    uint   MaterialIndex;
    uint   LightFlags;
    float2 _Pad;
    float4 Position;
    // Spot and point only, the direction the light points.
    float4 Forward;
    // w is the projection scale, as in FLightFunction.
    float4 Right;
    float4 Up;
};

FLightFunctionArgs* Pass() { return GetPassArgs<FLightFunctionArgs>(); }

// The world-space direction the light travels through this tile texel, the inverse of LightFunctionTileUV.
float3 LightFunctionTexelDirection(float2 TileUV)
{
    if (HasFlag(Pass().LightFlags, ELightFlags::Directional))
    {
        return Pass().Forward.xyz;
    }
    if (HasFlag(Pass().LightFlags, ELightFlags::Spot))
    {
        const float2 Plane = (TileUV * 2.0 - 1.0) / max(Pass().Right.w, 1e-4);
        return normalize(Pass().Forward.xyz + Plane.x * Pass().Right.xyz + Plane.y * Pass().Up.xyz);
    }
    return OctDecode01(TileUV);
}

// A spot or point texel sits one meter out along its direction, and a sun texel on the plane through the world origin.
float3 LightFunctionTexelPosition(float2 TileUV, float3 Direction)
{
    if (HasFlag(Pass().LightFlags, ELightFlags::Directional))
    {
        const float2 Plane = TileUV / max(Pass().Right.w, 1e-6);
        return Plane.x * Pass().Right.xyz + Plane.y * Pass().Up.xyz;
    }
    return Pass().Position.xyz + Direction;
}

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    uint   MaterialIndex = Pass().MaterialIndex;
    float2 UV0           = Input.UV;

    // The light's direction stands in for the normal, so graphs can mask by where the light points.
    float3 WorldNormal   = LightFunctionTexelDirection(UV0);
    float3 WorldPosition = LightFunctionTexelPosition(UV0, WorldNormal);
    // No vertex graph in this domain, so there is no offset to exclude.
    float3 WorldPositionExcludingOffsets = WorldPosition;
    float3 ViewPosition  = float3(0.0);
    float4 VertexColor   = float4(1.0);
    uint   EntityID      = 0;
    float4 WorldTangent  = float4(1.0, 0.0, 0.0, 1.0);
    // One tile parameterization, so set 1 aliases it.
    float2 UV1           = UV0;
    float2 UV1_DDX       = ddx(UV1);
    float2 UV1_DDY       = ddy(UV1);
    float2 UV0_DDX       = ddx(UV0);
    float2 UV0_DDY       = ddy(UV0);
    float3 WorldPosition_DDX = ddx(WorldPosition);
    float3 WorldPosition_DDY = ddy(WorldPosition);
    float3 WorldPositionExcludingOffsets_DDX = WorldPosition_DDX;
    float3 WorldPositionExcludingOffsets_DDY = WorldPosition_DDY;
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    $MATERIAL_INPUTS

    return float4(saturate(Material.Emissive), 1.0);
}
