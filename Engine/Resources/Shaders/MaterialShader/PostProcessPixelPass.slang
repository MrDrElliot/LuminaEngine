#define MATERIAL_NO_MIP_BIAS 1
#include "Includes/SceneGlobals.slang"

// The compiler substitutes the first occurrence of the material-graph token, so never spell it in a comment above its use.
struct FSceneTex
{
    FTexture2D Tex;
    FSampler   Smp;
    float4 Sample(float2 uv)               { return Tex.Sample(Smp, uv); }
    float4 SampleLevel(float2 uv, float l)  { return Tex.SampleLevel(Smp, uv, l); }
};

struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

struct FPostProcessPushConstants
{
    uint       MaterialIndex;
    FTexture2D SceneColor;      // ping-pong source (linear-clamp)
    FTexture2D SceneDepth;      // opaque depth (point-clamp)
    FTexture2D HDR;             // pre-tone-map HDR (linear-clamp)
};

FPostProcessPushConstants* Pass() { return GetPassArgs<FPostProcessPushConstants>(); }

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    // Scene inputs as wrappers named to match the codegen's `uSceneColor.Sample(uv)` references.
    FSceneTex uSceneColor;    uSceneColor.Tex    = Pass().SceneColor; uSceneColor.Smp    = Samplers::LinearClamp;
    FSceneTex uSceneDepth;    uSceneDepth.Tex    = Pass().SceneDepth; uSceneDepth.Smp    = Samplers::PointClamp;
    FSceneTex uHDRSceneColor; uHDRSceneColor.Tex = Pass().HDR;        uHDRSceneColor.Smp = Samplers::LinearClamp;

    uint   MaterialIndex = Pass().MaterialIndex;
    float2 UV0           = Input.UV;
    float4 SceneColor    = uSceneColor.Sample(Input.UV);

    // Zeroed because no rasterized geometry feeds this pass, but node code may still reference them.
    float3 WorldPosition = float3(0.0);
    // No vertex graph in this domain, so there is no offset to exclude.
    float3 WorldPositionExcludingOffsets = WorldPosition;
    float3 WorldNormal   = float3(0.0, 0.0, 1.0);
    float3 ViewPosition  = float3(0.0);
    float4 VertexColor   = float4(1.0);
    uint   EntityID      = 0;
    float4 WorldTangent  = float4(1.0, 0.0, 0.0, 1.0);
    // One screen parameterization, so set 1 aliases it.
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

    return float4(Material.Emissive, 1.0);
}
