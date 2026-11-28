// The compute lane has no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/TerrainData.slang"
#include "Includes/VirtualTexture.slang"

// Evaluates a terrain material straight down into a runtime virtual texture, one texel per lane, so grass and other surfaces can sample how the ground looks.

// Must match FVirtualTextureCaptureArgs in SceneRenderer_VirtualTexture.cpp.
struct FVirtualTextureCaptureArgs
{
    FRWTexture2D Target;
    uint         Resolution;
    float        TexelSize;
    uint         TerrainIndex;
    int2         RectMin;
    uint2        RectSize;
    uint         Layout;
    float        Mip;
};
FVirtualTextureCaptureArgs* Pass() { return GetPassArgs<FVirtualTextureCaptureArgs>(); }

FTerrainVertex DisplacedTerrainVertex(FTerrainRenderParams TerrainParams, FTerrainVertex V)
{
    float3 WorldPos = V.WorldPos;
    float3 NormalWS = V.NormalWS;
    float2 HeightUV = V.HeightUV;
    FMaterialVertexInputs Material;
    $MATERIAL_VERTEX_INPUTS
    V.WorldPos += Material.WorldPositionOffset;
    return V;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= Pass().RectSize))
    {
        return;
    }

    GTerrainIndex = Pass().TerrainIndex;
    FTerrainRecord* Terrain = TerrainArgs();
    const int2   WorldTexel = Pass().RectMin + int2(DTid.xy);
    const float  Texel      = Pass().TexelSize;
    const float2 XZ         = (float2(WorldTexel) + 0.5) * Texel;
    if (!TerrainCovers(Terrain, XZ))
    {
        return;
    }

    const FTerrainRenderParams Params = GetTerrainParams();
    const float2 Sample   = TerrainSamplePos(Terrain, XZ);
    const float2 HeightUV = (Sample + 0.5) / float(Params.Resolution);
    GTerrainShadingMip = Pass().Mip;

    FTerrainPage Cache = (FTerrainPage)0;
    const float  Height   = TerrainHeightAt(Terrain, Sample, (uint)Pass().Mip, Cache);
    const float3 NormalWS = TerrainSampleNormalAt(HeightUV);
    const float3 Position = float3(XZ.x, Params.WorldOriginY.x + Height, XZ.y);

    FBaseVertexOutput Input;
    Input.FragColor     = float4(1.0, 1.0, 1.0, 1.0);
    Input.NormalVS      = float4(normalize(NormalWorldToView(NormalWS)), 0.0);
    Input.NormalWS      = float4(NormalWS, 0.0);
    Input.TangentWS     = float4(TerrainTangent(NormalWS), 1.0);
    Input.FragPos       = CameraViewPosition(Position);
    Input.WorldPosNoWPO = Position;
    Input.UV            = XZ;
    Input.UV1           = XZ;
    Input.InstanceIndex = 0u;
    Input.EntityID      = Params.EntityID;
    Input.MaterialIndex = Params.MaterialIndex;
    Input.InstanceFlags = (uint)EInstanceFlags::ReceiveShadow;
    Input.MeshletIndex  = 0u;
    Input.Position      = float4(0.0, 0.0, 0.5, 1.0);

    // Locals named as the pixel graph codegen reads them, with one texel as the footprint the gradients describe.
    uint   EntityID       = Input.EntityID;
    uint   MaterialIndex  = Input.MaterialIndex;
    bool   bReceiveShadow = true;
    float3 WorldPosition  = Position;
    float3 WorldPositionExcludingOffsets = Position;
    float3 WorldNormal    = NormalWS;
    float4 WorldTangent   = Input.TangentWS;
    float2 UV0            = XZ;
    float2 UV1            = XZ;
    float4 VertexColor    = Input.FragColor;
    float3 ViewPosition   = Input.FragPos.xyz;
    float2 UV0_DDX        = float2(Texel, 0.0);
    float2 UV0_DDY        = float2(0.0, Texel);
    float2 UV1_DDX        = UV0_DDX;
    float2 UV1_DDY        = UV0_DDY;
    float3 WorldPosition_DDX = float3(Texel, 0.0, 0.0);
    float3 WorldPosition_DDY = float3(0.0, 0.0, Texel);
    float3 WorldPositionExcludingOffsets_DDX = WorldPosition_DDX;
    float3 WorldPositionExcludingOffsets_DDY = WorldPosition_DDY;
    float4 VertexColor_DDX   = float4(0.0, 0.0, 0.0, 0.0);
    float4 VertexColor_DDY   = float4(0.0, 0.0, 0.0, 0.0);

    $MATERIAL_INPUTS

    const float  Height01 = abs(Params.MaxHeight) > 0.0 ? saturate(Height / Params.MaxHeight) : 0.0;
    const float4 Value    = VirtualTextureCaptureValue(Pass().Layout, Material.Diffuse, Material.Roughness, NormalWS, Height01);
    Pass().Target[VirtualTextureImageTexel(WorldTexel, Pass().Resolution)] = Value;
}
