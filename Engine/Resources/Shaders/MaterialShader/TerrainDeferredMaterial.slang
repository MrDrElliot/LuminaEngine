// The compute lane has no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/TerrainData.slang"
#include "Includes/DeferredMaterialCommon.slang"

// Shades a terrain material's VisBuffer pixels, rebuilding each triangle from the heightmap the way TerrainVisBuffer rastered it.

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
[numthreads(MATERIAL_PIXEL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    uint2 Texel;
    uint  LinearGroup;
    if (!FindDeferredTexel(GroupID, Lane, Texel, LinearGroup))
    {
        return;
    }

    const FTerrainVisTriangle T = ResolveTerrainVisTriangle(Pass().VisBuffer.Load(int2(Texel)));

    FGBufferData G = DefaultGBufferData();
    if (T.bValid)
    {
        const FTerrainRenderParams Params  = GetTerrainParams();
        const uint3                Corners = TerrainTriangleCorners(T.Tri);

        FMeshletWorldVertex W[3];
        float3 FinalWorldPos[3];
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            const FTerrainVertex V = TerrainMeshletVertex(Params, T.Meshlet, T.Chunk, Corners[k]);
            W[k].WorldPos  = V.WorldPos;
            W[k].NormalWS  = V.NormalWS;
            W[k].TangentWS = float4(TerrainTangent(V.NormalWS), 1.0);
            // UV is world XZ like the old forward pass, and set 1 carries the heightmap UV so it gets exact gradients too.
            W[k].UV        = V.WorldPos.xz;
            W[k].UV1       = V.HeightUV;
            W[k].Color     = float4(1.0, 1.0, 1.0, 1.0);
            FinalWorldPos[k] = DisplacedTerrainVertex(Params, V).WorldPos;
        }

        const float2 ScreenSize = float2(Pass().ScreenW, Pass().ScreenH);
        FVisSurface  S          = InterpolateVisSurface(W, FinalWorldPos, VisPixelNdc(Texel, ScreenSize), ScreenSize);

        // Per pixel, since a coarse LOD triangle interpolating three vertex normals lights whole facets as one.
        const float3 NormalWS = TerrainSampleNormalAt(S.UV1);
        const float4 ViewPos  = CameraViewPosition(S.WorldPosition);
        const float4 ClipPos  = CameraClipPosition(ViewPos);
        const float  InvW     = 1.0 / max(ClipPos.w, 1e-6);

        FBaseVertexOutput Input;
        Input.FragColor     = float4(1.0, 1.0, 1.0, 1.0);
        Input.NormalVS      = float4(normalize(NormalWorldToView(NormalWS)), 0.0);
        Input.NormalWS      = float4(NormalWS, 0.0);
        Input.TangentWS     = float4(TerrainTangent(NormalWS), 1.0);
        Input.FragPos       = ViewPos;
        Input.WorldPosNoWPO = S.WorldPositionNoOffset;
        Input.UV            = S.UV;
        Input.UV1           = S.UV;
        Input.InstanceIndex = 0u;
        Input.EntityID      = Params.EntityID;
        Input.MaterialIndex = Params.MaterialIndex;
        Input.InstanceFlags = (uint)EInstanceFlags::ReceiveShadow;
        Input.MeshletIndex  = 0u;
        Input.Position      = float4(float2(Texel) + 0.5, ClipPos.z * InvW, InvW);

        // Locals named as the pixel graph codegen reads them.
        uint   EntityID       = Input.EntityID;
        uint   MaterialIndex  = Input.MaterialIndex;
        bool   bReceiveShadow = true;
        float3 WorldPosition  = S.WorldPosition;
        float3 WorldPositionExcludingOffsets = S.WorldPositionNoOffset;
        float3 WorldNormal    = NormalWS;
        float4 WorldTangent   = Input.TangentWS;
        float2 UV0            = S.UV;
        float2 UV1            = S.UV;
        float2 HeightUV       = S.UV1;
        float4 VertexColor    = Input.FragColor;
        float3 ViewPosition   = ViewPos.xyz;
        float2 UV0_DDX        = S.UVDDX;
        float2 UV0_DDY        = S.UVDDY;
        float2 UV1_DDX        = S.UVDDX;
        float2 UV1_DDY        = S.UVDDY;
        float3 WorldPosition_DDX = InterpWeights(S.Bary.DDX, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float3 WorldPosition_DDY = InterpWeights(S.Bary.DDY, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float3 WorldPositionExcludingOffsets_DDX = InterpWeights(S.Bary.DDX, W[0].WorldPos, W[1].WorldPos, W[2].WorldPos);
        float3 WorldPositionExcludingOffsets_DDY = InterpWeights(S.Bary.DDY, W[0].WorldPos, W[1].WorldPos, W[2].WorldPos);
        float4 VertexColor_DDX   = float4(0.0, 0.0, 0.0, 0.0);
        float4 VertexColor_DDY   = float4(0.0, 0.0, 0.0, 0.0);

        // Not named WorldNormal_DDX so it stays out of the graph's alias set, and used only for specular AA.
        const float3 GeoNormalDDX = InterpWeights(S.Bary.DDX, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);
        const float3 GeoNormalDDY = InterpWeights(S.Bary.DDY, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);

        // Group-uniform so the wave ops in RequestMaterialResolution stay convergent.
        if ((LinearGroup + Pass().FeedbackPhase) % STREAMING_FEEDBACK_WINDOW == 0u)
        {
            RequestMaterialResolution(MaterialIndex, UV0_DDX, UV0_DDY, UV1_DDX, UV1_DDY);
        }
        $MATERIAL_INPUTS

        G = BuildGBufferData(Input, Material);
        G.Roughness = ApplyGeometricSpecularAA(G.Roughness, GeoNormalDDX, GeoNormalDDY);
        G.ClearcoatRoughness = ApplyGeometricSpecularAA(G.ClearcoatRoughness, GeoNormalDDX, GeoNormalDDY);
    }

    WriteDeferredGBuffer(Texel, G);
}
