#include "Includes/TerrainData.slang"
#include "Includes/SurfaceShading.slang"

// Terrain base-pass pixel shader. Terrain keeps its own heightfield geometry (the meshlet-driven VS + its
// FTerrainPushConstants), but its SHADING is the shared ShadeSurface (SurfaceShading.slang) -- the exact
// path meshes use -- so lighting, shadows, IBL, clustering and the debug views can never drift from the
// mesh path. This file only: builds the material-graph locals, runs the pixel material graph (which reaches
// terrain layer blending via TerrainData.slang), adapts the terrain interpolants to the shared
// FBaseVertexOutput, and packs output. All terrain layer access lives in TerrainData.slang.
//
// The terrain pipeline is specialized with SPEC_ENABLE_DECALS = 0 and SPEC_ENABLE_GTAO = 0 (it binds
// neither the DBuffer overlay nor an GTAO input), so ShadeSurface's decal/AO blocks are dead-stripped and
// terrain never misreads its pointer-bearing push range as bindless decal indices.

struct PSInput
{
    float4 FragColor                : COLOR;
    float4 NormalVS                 : NORMAL_VS;
    float4 NormalWS                 : NORMAL_WS;
    float4 TangentWS                : TANGENT_WS;
    float4 FragPos                  : FRAG_POS;
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

// Masked materials run their alpha-test discard (inside ShadeSurface) which MUST execute before the depth
// write, so the masked variant omits forced early-Z. Non-masked keeps it over the heavy PBR shader; its
// discard branch is never taken so the early write stays correct. Mirrors BasePixelPass.slang.
#ifndef MASKED
[earlydepthstencil]
#endif
[shader("fragment")]
PSOutput FragmentMain(PSInput Input)
{
    // Locals the material pixel graph codegen reads (terrain aliases UV0 -> world XZ, plus HeightUV for the
    // layer-weight lookups in TerrainData.slang).
    uint   EntityID      = Input.EntityID;
    uint   MaterialIndex = Input.MaterialIndex;
    float3 WorldNormal   = normalize(Input.NormalWS.xyz);
    float3 ViewPosition  = Input.FragPos.xyz;
    float3 WorldPosition = mul(GetInverseCameraView(), float4(Input.FragPos.xyz, 1.0)).xyz;
    float2 UV0           = Input.UV;
    float2 HeightUV      = Input.HeightUV;
    float4 VertexColor   = Input.FragColor;
    float4 WorldTangent  = Input.TangentWS;
    // Terrain parameterizes from world XZ and has no authored second set; aliasing keeps a material that
    // samples set 1 working instead of collapsing it to zero.
    float2 UV1           = UV0;
    float2 UV1_DDX       = ddx(UV1);
    float2 UV1_DDY       = ddy(UV1);
    float2 UV0_DDX       = ddx(UV0);
    float2 UV0_DDY       = ddy(UV0);
    float3 WorldPosition_DDX = ddx(WorldPosition);
    float3 WorldPosition_DDY = ddy(WorldPosition);
    float4 VertexColor_DDX   = ddx(VertexColor);
    float4 VertexColor_DDY   = ddy(VertexColor);

    // Same as BasePixelPass: implicit sampling kept, plus a streaming request. This lane matters most of
    // all -- the terrain layer ARRAYS are sampled here, and nothing else samples them.
#define SampleTexture2DAuto(TexIndex, SamplerIndex, UV, DDX, DDY) SampleTexture2DStreamedAuto((TexIndex), (SamplerIndex), (UV))
#define SampleTexture2DArrayAuto(TexIndex, SamplerIndex, UV, Slice, DDX, DDY) SampleTexture2DArrayStreamedAuto((TexIndex), (SamplerIndex), (UV), (Slice))
// A custom Slang node can call the explicit-gradient entry points by name instead of going through the
// Auto macro above. Redirect those too, or such a material samples correctly but reports nothing and its
// textures never leave their inline tail. Safe from recursion: SceneGlobals.slang is included above this
// point, so the ...Streamed bodies were already text-substituted before these defines exist.
#define SampleTexture2DGrad(TexIndex, SamplerIndex, UV, DDX, DDY) SampleTexture2DStreamed((TexIndex), (SamplerIndex), (UV), (DDX), (DDY))
#define SampleTexture2DArrayGrad(TexIndex, SamplerIndex, UV, Slice, DDX, DDY) SampleTexture2DArrayStreamed((TexIndex), (SamplerIndex), (UV), (Slice), (DDX), (DDY))
#define SampleTexture2DLevel(TexIndex, SamplerIndex, UV, Lod) SampleTexture2DLevelStreamed((TexIndex), (SamplerIndex), (UV), (Lod))
#define SampleTexture2DArrayLevel(TexIndex, SamplerIndex, UV, Slice, Lod) SampleTexture2DArrayLevelStreamed((TexIndex), (SamplerIndex), (UV), (Slice), (Lod))
    $MATERIAL_INPUTS
#undef SampleTexture2DArrayLevel
#undef SampleTexture2DLevel
#undef SampleTexture2DArrayGrad
#undef SampleTexture2DGrad
#undef SampleTexture2DArrayAuto
#undef SampleTexture2DAuto

    // Adapt the terrain interpolants to the shared shading input. Terrain has no per-instance data, so the
    // instance fields carry constants: ReceiveShadow stays on (terrain has always received sun shadows),
    // MeshletIndex feeds the meshlet debug view.
    FBaseVertexOutput Surface;
    Surface.FragColor     = Input.FragColor;
    Surface.NormalVS      = Input.NormalVS;
    Surface.NormalWS      = Input.NormalWS;
    Surface.TangentWS     = Input.TangentWS;
    Surface.FragPos       = Input.FragPos;
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
