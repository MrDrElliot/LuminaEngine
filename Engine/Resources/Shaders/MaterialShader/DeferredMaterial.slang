#include "Includes/SurfaceShading.slang"

// Deferred VisBuffer material pass. Runs over the tiles this material covers (see DeferredMaterialTileVS):
// read this pixel's triangle ID from the VisBuffer, re-fetch the meshlet triangle, reconstruct
// perspective-correct attributes + analytic UV derivatives via barycentric coordinates, run the material
// graph, and shade through the shared ShadeSurface (identical lighting to the forward base pass). Replaces
// the forward base pass entirely: every visible pixel is owned by exactly one triangle and shaded once.
//
// Per-material because the geometry reconstruction re-applies the material's WorldPositionOffset (so the
// reconstructed triangle matches what the VisBuffer geometry pass rasterized) and runs the material's
// pixel graph. The VisID packing matches MeshletVisBuffer/MeshletVisBufferVS: [31:7] global meshlet slot,
// [6:0] local triangle index.

// Layout overlays FDBufferPushConstants (DBuffer.slang) on its first 16 bytes so the shared ShadeSurface ->
// ApplyDBuffer reads the decal indices from the same push constant: offset 0 is read by ApplyDBuffer as an
// (unused) MeshletBase, and the three DBuffer indices sit at offsets 4/8/12.
//
// Shared with DeferredMaterialTileVS.slang (same pipeline, same push block) -- the fields from SlotIndex on
// are the vertex shader's; keep both declarations and the C++ FDeferredPushConstants byte-identical.
struct FDeferredPassArgs
{
    uint  VisBufferIndex;   // 0   bindless R32_UINT visibility target
    uint  DBufferAIndex;    // 4   overlaid by FDBufferPushConstants in ApplyDBuffer
    uint  DBufferBIndex;    // 8
    uint  DBufferCIndex;    // 12
    uint  DrawListCount;    // 16  total MeshletDrawList entries; slot bound
    uint  SlotIndex;        // 20  dense deferred-draw slot (VS: encodes this draw's depth)
    uint  TileCountX;       // 24  VS
    uint  TileSizePx;       // 28  VS
    uint  ScreenW;          // 32  VS
    uint  ScreenH;          // 36  VS
    uint* TileList;         // 40  VS: compacted per-slot tile list
};
FDeferredPassArgs* Pass() { return GetPassArgs<FDeferredPassArgs>(); }

// Perspective-correct barycentrics + their screen-space derivatives, from the triangle's three clip-space
// positions and the pixel's NDC location (Schied/Stachowiak, "Visibility Buffer Rendering with Material
// Graphs"). DDX/DDY are d(lambda)/d(screen pixel), used to take analytic gradients of any interpolated
// attribute -- no rasterizer ddx/ddy, so texture mips stay correct across triangle boundaries.
struct FBaryDeriv
{
    float3 Lambda;
    float3 DDX;
    float3 DDY;
};

FBaryDeriv CalcFullBary(float4 Pt0, float4 Pt1, float4 Pt2, float2 PixelNdc, float2 WinSize)
{
    FBaryDeriv Ret;
    float3 InvW = rcp(float3(Pt0.w, Pt1.w, Pt2.w));

    float2 Ndc0 = Pt0.xy * InvW.x;
    float2 Ndc1 = Pt1.xy * InvW.y;
    float2 Ndc2 = Pt2.xy * InvW.z;

    float InvDet = rcp(determinant(float2x2(Ndc2 - Ndc1, Ndc0 - Ndc1)));
    Ret.DDX = float3(Ndc1.y - Ndc2.y, Ndc2.y - Ndc0.y, Ndc0.y - Ndc1.y) * InvDet * InvW;
    Ret.DDY = float3(Ndc2.x - Ndc1.x, Ndc0.x - Ndc2.x, Ndc1.x - Ndc0.x) * InvDet * InvW;
    float DDXSum = dot(Ret.DDX, float3(1.0, 1.0, 1.0));
    float DDYSum = dot(Ret.DDY, float3(1.0, 1.0, 1.0));

    float2 DeltaVec  = PixelNdc - Ndc0;
    float InterpInvW = InvW.x + DeltaVec.x * DDXSum + DeltaVec.y * DDYSum;
    float InterpW    = rcp(InterpInvW);

    Ret.Lambda.x = InterpW * (InvW.x + DeltaVec.x * Ret.DDX.x + DeltaVec.y * Ret.DDY.x);
    Ret.Lambda.y = InterpW * (0.0    + DeltaVec.x * Ret.DDX.y + DeltaVec.y * Ret.DDY.y);
    Ret.Lambda.z = InterpW * (0.0    + DeltaVec.x * Ret.DDX.z + DeltaVec.y * Ret.DDY.z);

    Ret.DDX *= (2.0 / WinSize.x);
    Ret.DDY *= (2.0 / WinSize.y);
    DDXSum  *= (2.0 / WinSize.x);
    DDYSum  *= (2.0 / WinSize.y);

    // NDC y points opposite screen y.
    Ret.DDY *= -1.0;
    DDYSum  *= -1.0;

    float InterpWDDX = 1.0 / (InterpInvW + DDXSum);
    float InterpWDDY = 1.0 / (InterpInvW + DDYSum);

    Ret.DDX = InterpWDDX * (Ret.Lambda * InterpInvW + Ret.DDX) - Ret.Lambda;
    Ret.DDY = InterpWDDY * (Ret.Lambda * InterpInvW + Ret.DDY) - Ret.Lambda;

    return Ret;
}

float3 InterpBary(FBaryDeriv D, float3 V0, float3 V1, float3 V2)
{
    return V0 * D.Lambda.x + V1 * D.Lambda.y + V2 * D.Lambda.z;
}

float4 InterpBary4(FBaryDeriv D, float4 V0, float4 V1, float4 V2)
{
    return V0 * D.Lambda.x + V1 * D.Lambda.y + V2 * D.Lambda.z;
}

struct PSOutput
{
    float4 Color  : SV_Target0;
    uint   Picker : SV_Target1;
};

[shader("fragment")]
PSOutput FragmentMain(float4 SvPosition : SV_Position)
{
    int2 Texel = int2(SvPosition.xy);

    PSOutput Output;
    Output.Color  = float4(0.0, 0.0, 0.0, 0.0);
    Output.Picker = 0u;

    // No binning reject here. Every pixel that reaches this shader was already proven to belong to this
    // draw's slot: MaterialDepth stamped each covered pixel with its owning slot's encoded depth, the tile
    // quad is emitted at this slot's encoded depth, and the pipeline tests DEPTH_EQUAL -- so foreign pixels
    // are killed by fixed-function hardware before a shader launches. The shading below reads each pixel's
    // own per-instance MaterialIndex, so all instances of this master shade correctly in this one draw.
    uint Raw   = LoadTexture2DUInt(Pass().VisBufferIndex, Texel);

    uint SlotCount = Pass().DrawListCount;          // covers both camera views (early + late slices)
    uint InstCount = Scene().CullData.InstanceNum;

    uint VisID       = Raw - 1u;   // geometry stored VisID + 1
    uint MeshletSlot = VisID >> 7u;
    uint TriId       = VisID & 0x7Fu;

    // Indices are still clamped fault-safe. The depth test reaching this shader already implies a valid
    // resolve -- MaterialDepth derived this pixel's slot from this same VisBuffer texel and discarded if it
    // could not -- so this is belt-and-suspenders against a speculated buffer-device-address load faulting on
    // a stale draw-list gap. Cheap, and this pass has crashed the GPU on exactly that shape before.
    bool bValid   = (Raw != 0u) && (SlotCount != 0u) && (MeshletSlot < SlotCount);
    uint SafeSlot = bValid ? MeshletSlot : 0u;

    FMeshletDraw Draw = MeshletDrawList()[SafeSlot];
    // See MaterialDepth: reject entries this frame did not write instead of shading stale ones.
    bValid = bValid && Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) && (Draw.InstanceID < InstCount);
    uint SafeInst = (Draw.InstanceID < InstCount) ? Draw.InstanceID : 0u;

    // Build the meshlet ref with clamped indices (mirrors ResolveSurfaceMeshlet, but fault-proof against a
    // stale draw-list gap whose InstanceID/MeshletLocalIndex are garbage).
    FMeshletRef Ref;
    Ref.Inst              = GetInstance(SafeInst);
    Ref.InstanceID        = SafeInst;
    Ref.SkinnedVertexBase = Ref.Inst.SkinnedVertexBase;   // deferred pass is camera-side only
    uint LocalCount       = max(Ref.Inst.SurfaceMeshletCount, 1u);
    uint SafeLocal        = min(Draw.MeshletLocalIndex, LocalCount - 1u);
    Ref.GlobalMeshletIdx  = Ref.Inst.SurfaceMeshletOffset + SafeLocal;
    Ref.M                 = Ref.Inst.MeshletHeader.Meshlets[Ref.GlobalMeshletIdx];

    if (!bValid)
    {
        discard;
        return Output;
    }

    uint SafeTri = min(TriId, max(Ref.M.TriangleCount, 1u) - 1u);
    uint3 Tri = UnpackMeshletTriangle(Ref, SafeTri);

    // Load + model->world transform the three triangle vertices.
    FVertexData Vd[3];
    Vd[0] = LoadMeshletVertexData(Ref, Tri.x);
    Vd[1] = LoadMeshletVertexData(Ref, Tri.y);
    Vd[2] = LoadMeshletVertexData(Ref, Tri.z);

    // The three corners share the instance's ModelMatrix, so derive the world normal matrix once.
    float3x3 NormalMatrixWS = CalculateNormalMatrix(Ref.Inst.ModelMatrix);
    FMeshletWorldVertex W[3];
    W[0] = TransformMeshletVertex(Ref, Vd[0], NormalMatrixWS);
    W[1] = TransformMeshletVertex(Ref, Vd[1], NormalMatrixWS);
    W[2] = TransformMeshletVertex(Ref, Vd[2], NormalMatrixWS);

    // Re-apply the material vertex graph (WorldPositionOffset) per vertex so the reconstructed triangle
    // matches what the VisBuffer geometry pass rasterized. The token appears once; the unrolled loop runs
    // it for all three corners (same local contract as the geometry shaders). Inst/ModelMatrix are
    // loop-invariant; hoist them so the unroll doesn't materialize the 64-byte BDA matrix three times
    // (SPIR-V LICM doesn't hoist BDA loads -- see the FLightShadow note in SurfaceShading).
    FGPUInstance Inst    = Ref.Inst;
    float4x4 ModelMatrix = Inst.ModelMatrix;
    float3 FinalWorldPos[3];
    [unroll]
    for (int k = 0; k < 3; ++k)
    {
        float4   WorldPos    = float4(W[k].WorldPos, 1.0);
        float3   NormalWS    = W[k].NormalWS;
        float3   TangentWS   = W[k].TangentWS.xyz;
        FVertexData VertexData = Vd[k];

        FMaterialVertexInputs Material;
        $MATERIAL_VERTEX_INPUTS
        FinalWorldPos[k] = W[k].WorldPos + Material.WorldPositionOffset;
    }

    // Clip-space positions of the displaced triangle, then the pixel's barycentric reconstruction.
    float4 C0 = CameraClipPosition(CameraViewPosition(FinalWorldPos[0]));
    float4 C1 = CameraClipPosition(CameraViewPosition(FinalWorldPos[1]));
    float4 C2 = CameraClipPosition(CameraViewPosition(FinalWorldPos[2]));

    float2 ScreenSize = float2(Scene().ScreenSize.xy);
    float2 PixelNdc   = (SvPosition.xy / ScreenSize) * 2.0 - 1.0;

    FBaryDeriv Bary = CalcFullBary(C0, C1, C2, PixelNdc, ScreenSize);

    // Perspective-correct interpolated surface attributes.
    float3 WorldPosition = InterpBary(Bary, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
    float3 NormalWS      = InterpBary(Bary, W[0].NormalWS, W[1].NormalWS, W[2].NormalWS);
    float3 TangentXYZ    = InterpBary(Bary, W[0].TangentWS.xyz, W[1].TangentWS.xyz, W[2].TangentWS.xyz);
    float4 VertexColor   = InterpBary4(Bary, W[0].Color, W[1].Color, W[2].Color);

    // Two-sided materials rasterize back faces into the VisBuffer too (VisBufferPass disables back-face cull
    // for them). There is no SV_IsFrontFace in a fullscreen deferred pass, so flip the shading normal to face
    // the camera on back faces; the tangent basis is re-orthogonalized against it downstream.
    if (Ref.Inst.HasFlag(EInstanceFlags::TwoSided))
    {
        float3 ViewDir = normalize(GetCameraPosition() - WorldPosition);
        if (dot(NormalWS, ViewDir) < 0.0)
        {
            NormalWS = -NormalWS;
        }
    }

    float3 UVx = float3(W[0].UV.x, W[1].UV.x, W[2].UV.x);
    float3 UVy = float3(W[0].UV.y, W[1].UV.y, W[2].UV.y);
    float2 UV0    = float2(dot(UVx, Bary.Lambda), dot(UVy, Bary.Lambda));
    float2 dUVdx  = float2(dot(UVx, Bary.DDX),    dot(UVy, Bary.DDX));
    float2 dUVdy  = float2(dot(UVx, Bary.DDY),    dot(UVy, Bary.DDY));

    float4 ViewPos = CameraViewPosition(WorldPosition);

    // Rebuild the shared interpolant struct ShadeSurface consumes.
    FBaseVertexOutput Input;
    Input.FragColor     = VertexColor;
    Input.NormalVS      = float4(normalize(NormalWorldToView(NormalWS)), 1.0);
    Input.NormalWS      = float4(NormalWS, 1.0);
    Input.TangentWS     = float4(TangentXYZ, W[0].TangentWS.w);
    Input.FragPos       = ViewPos;
    Input.UV            = UV0;
    Input.InstanceIndex = Ref.InstanceID;
    Input.EntityID      = Ref.Inst.EntityID;
    Input.MaterialIndex = Ref.Inst.MaterialIndex;
    Input.InstanceFlags = (uint)Ref.Inst.Flags;
    Input.MeshletIndex  = Ref.GlobalMeshletIdx;
    Input.Position      = SvPosition;

    // Material pixel graph locals (the names the pixel graph codegen reads).
    uint EntityID        = Input.EntityID;
    uint MaterialIndex   = Input.MaterialIndex;
    bool bReceiveShadow  = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
    float3 WorldNormal   = normalize(NormalWS);
    float3 ViewPosition  = ViewPos.xyz;

    // Graph local contract (see BasePixelPass). The analytic gradients are the whole reason a
    // tangent-space marcher works in this lane -- implicit ddx/ddy across a fullscreen tile quad
    // would pick a mip from unrelated triangles.
    float4 WorldTangent  = Input.TangentWS;
    float2 UV0_DDX       = dUVdx;
    float2 UV0_DDY       = dUVdy;

    // Deferred-texturing fix: redirect the codegen's UV0 texture samples to explicit-gradient sampling using
    // the analytic perspective-correct gradients computed above. A fullscreen deferred PS has no valid 2x2
    // quad (it straddles unrelated triangles), so implicit ddx/ddy pick wrong mips along every silhouette,
    // meshlet seam, and instance edge -- the canonical VisBuffer pitfall. Exact for UV0-direct samples (the
    // common case); derived UVs (tiling/panners) use UV0's gradient -- bounded and far better than garbage.
    // Scoped to the material graph only: ShadeSurface (below) and the forward/masked raster passes keep
    // implicit derivatives, which are correct there.
#define SampleTexture2D(TexIndex, SamplerIndex, UV) SampleTexture2DGrad((TexIndex), (SamplerIndex), (UV), dUVdx, dUVdy)
    $MATERIAL_INPUTS
#undef SampleTexture2D

    FShadedSurface Shaded = ShadeSurface(Input, Material);

    // VisBuffer-specific debug view: a unique stable hue per reconstructed triangle (per-meshlet base +
    // local triangle index). Material ID and the other generic views are handled inside ShadeSurface; the
    // triangle identity only exists here in the deferred reconstruction. Spec-gated so game/shipping strips it.
    float3 OutColor = Shaded.Color;
    if (SPEC_ENABLE_DEBUG_VIEWS != 0 && Scene().CullData.DebugMode == DEBUG_MODE_TRIANGLE_ID)
    {
        OutColor = MeshletDebugColor(Ref.GlobalMeshletIdx * 131u + SafeTri);
    }

    Output.Color  = float4(OutColor, Shaded.Alpha);
    Output.Picker = Input.EntityID;
    return Output;
}
