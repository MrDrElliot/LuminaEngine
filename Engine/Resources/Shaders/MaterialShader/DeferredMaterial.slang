// Compute lane: no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Wind.slang"   // this template substitutes the material vertex token too (WPO reconstruction)

// Per-material GBuffer generation -- step 5 of the visibility-buffer pipeline.
//
// One indirect dispatch per material, over exactly the pixels the classify passes proved that material
// owns: read the pixel's screen position out of the compacted list, re-fetch its triangle from the VisID,
// reconstruct perspective-correct attributes with analytic UV gradients, run the material graph, and store
// the result in the GBuffer. Lighting happens later, once, in DeferredLighting.slang.
//
// COMPUTE, not a rasterized tile quad, is the whole point of the design: a pixel shader launches in 2x2
// quads, so a triangle covering one pixel still pays four lanes and three of them are helpers. Here every
// thread is a pixel a material actually owns, so the material graph runs exactly once per pixel no matter
// how dense the geometry is. It is also why the derivatives must be analytic -- there is no quad to take
// them from, and neighboring threads in this dispatch belong to unrelated triangles.
//
// Per-material because the reconstruction re-applies this material's WorldPositionOffset (so the rebuilt
// triangle is the one the geometry pass rasterized) and runs its pixel graph.
//
// Masked materials clip in the GEOMETRY stage, so a cut-out pixel never reaches the VisBuffer and never
// enters this list. There is no alpha test here, and a compute shader could not discard anyway.

// Offsets 0-12 overlay FDBufferPushConstants (DBuffer.slang) so the shared BuildGBufferData -> ApplyDBuffer
// reads the decal indices out of this same block: offset 0 is read there as an (unused) MeshletBase.
struct FDeferredMaterialArgs
{
    uint  VisBufferIndex;   // 0
    uint  DBufferAIndex;    // 4
    uint  DBufferBIndex;    // 8
    uint  DBufferCIndex;    // 12
    uint  DrawListCount;    // 16  MeshletDrawList slot bound
    uint  SlotIndex;        // 20  this dispatch's dense material slot
    uint  ScreenW;          // 24
    uint  ScreenH;          // 28
    uint  GBufferAUAV;      // 32  storage-heap slots, channel order per Includes/GBuffer.slang
    uint  GBufferBUAV;      // 36
    uint  GBufferCUAV;      // 40
    uint  GBufferDUAV;      // 44
    uint* PixelList;        // 48  packed screen positions, one contiguous run per slot
    uint* Starts;           // 56  uint[MATERIAL_MAX_SLOTS]
    uint* Counts;           // 64  uint[MATERIAL_MAX_SLOTS]
};
FDeferredMaterialArgs* Pass() { return GetPassArgs<FDeferredMaterialArgs>(); }

[shader("compute")]
[numthreads(MATERIAL_PIXEL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    const uint Slot = Pass().SlotIndex;

    // The grid is folded across Y because a 4K frame's pixel count overruns maxComputeWorkGroupCount[0];
    // VisBufferMaterialPrefixSum built it with the matching fold.
    const uint LinearGroup = GroupID.y * MAX_DISPATCH_AXIS + GroupID.x;
    const uint Index       = LinearGroup * MATERIAL_PIXEL_GROUP_SIZE + Lane;
    if (Index >= Pass().Counts[Slot])
    {
        return;
    }

    const uint2 Texel = UnpackVisPixel(Pass().PixelList[Pass().Starts[Slot] + Index]);

    const uint   Raw = LoadTexture2DUInt(Pass().VisBufferIndex, int2(Texel));
    FVisTriangle T   = ResolveVisTriangle(Raw, Pass().DrawListCount);

    FGBufferData G = DefaultGBufferData();
    if (T.bValid)
    {
        // The three corners share the instance's ModelMatrix, so load it ONCE and derive the world normal
        // matrix from that local. Reaching it through Ref.Inst per corner is a PhysicalStorageBuffer load
        // through a pointer that may alias anything, so each use would be a fresh dependent fetch that
        // SPIR-V LICM will not merge.
        const float4x4 ModelMatrix    = T.Ref.Inst.ModelMatrix;
        float3x3       NormalMatrixWS = CalculateNormalMatrix(ModelMatrix);

        // Re-apply the material vertex graph (WorldPositionOffset) per corner so the reconstructed triangle
        // matches what the VisBuffer geometry pass rasterized. The material vertex token appears once; the
        // unrolled loop runs it for all three corners.
        //
        // The LOAD and TRANSFORM are fused into this loop deliberately: hoisted, an FVertexData[3] (48
        // VGPRs) stays live across the whole graph body purely to supply the alias preamble's UV/Color,
        // which TransformMeshletVertex already copies into W. Fused, one corner's FVertexData is live at a
        // time. Registers are the binding resource in this pass.
        FGPUInstance* Inst = T.Ref.Inst;   // reached, not stored -- see FMeshletRef
        FMeshletWorldVertex W[3];
        float3 FinalWorldPos[3];
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            FVertexData VertexData = LoadMeshletVertexData(T.Ref, T.Tri[k]);
            W[k] = TransformMeshletVertex(VertexData, ModelMatrix, NormalMatrixWS);

            float4 WorldPos      = float4(W[k].WorldPos, 1.0);
            float3 NormalWS      = W[k].NormalWS;
            float3 TangentWS     = W[k].TangentWS.xyz;
            float  TangentSignWS = W[k].TangentWS.w;   // handedness; the graph's WorldTangent alias needs it

            FMaterialVertexInputs Material;
            $MATERIAL_VERTEX_INPUTS
            FinalWorldPos[k] = W[k].WorldPos + Material.WorldPositionOffset;
        }

        const float2 ScreenSize = float2(Pass().ScreenW, Pass().ScreenH);
        FVisSurface  S          = InterpolateVisSurface(W, FinalWorldPos, VisPixelNdc(Texel, ScreenSize), ScreenSize);

        const float3 OrientedNormalWS = OrientVisNormal(T.Ref, S.NormalWS, S.WorldPosition);
        const float4 ViewPos          = CameraViewPosition(S.WorldPosition);
        const float4 ClipPos          = CameraClipPosition(ViewPos);
        const float  InvW             = 1.0 / max(ClipPos.w, 1e-6);

        // The interpolant struct the shared material/shading code consumes, rebuilt from the reconstruction.
        FBaseVertexOutput Input;
        Input.FragColor     = S.VertexColor;
        Input.NormalVS      = float4(normalize(NormalWorldToView(OrientedNormalWS)), 1.0);
        Input.NormalWS      = float4(OrientedNormalWS, 1.0);
        Input.TangentWS     = S.TangentWS;
        Input.FragPos       = ViewPos;
        Input.UV            = S.UV;
        Input.InstanceIndex = T.Ref.InstanceID;
        Input.EntityID      = T.Ref.Inst.EntityID;
        Input.MaterialIndex = T.Ref.Inst.MaterialIndex;
        Input.InstanceFlags = (uint)T.Ref.Inst.Flags;
        Input.MeshletIndex  = T.Ref.GlobalMeshletIdx;
        // What the rasterizer would have handed a pixel shader here: pixel center, NDC z, 1/w. Material
        // nodes read .xy for screen position/UV and .z for pixel depth.
        Input.Position      = float4(float2(Texel) + 0.5, ClipPos.z * InvW, InvW);

        // Material pixel graph locals (the names the pixel graph codegen reads).
        uint   EntityID       = Input.EntityID;
        uint   MaterialIndex  = Input.MaterialIndex;
        bool   bReceiveShadow = (Input.InstanceFlags & (uint)EInstanceFlags::ReceiveShadow) != 0u;
        float3 WorldPosition  = S.WorldPosition;
        float3 WorldNormal    = normalize(OrientedNormalWS);
        float4 WorldTangent   = Input.TangentWS;
        float2 UV0            = S.UV;
        float2 UV1            = S.UV1;
        float4 VertexColor    = S.VertexColor;
        float3 ViewPosition   = ViewPos.xyz;
        float2 UV0_DDX        = S.UVDDX;
        float2 UV0_DDY        = S.UVDDY;
        // Set 1 carries its own barycentric gradients for the same reason set 0 does: there is no 2x2
        // quad in this lane, so an implicit derivative does not exist to fall back on.
        float2 UV1_DDX        = S.UV1DDX;
        float2 UV1_DDY        = S.UV1DDY;

        // Analytic derivatives of the other varyings a graph can build a UV out of -- world-space and
        // triplanar UVs are the common case, and without these they sample with UV0's gradient. Exact:
        // each is the same barycentric interpolation as the value above, taken with dLambda/dx instead.
        // No cost when unused; the whole chain dead-strips.
        //
        // WorldNormal deliberately has none. The value here is normalized and two-sided-flipped, so an
        // interpolated-normal derivative would not describe it, and a UV built from a normal is not a
        // real authoring pattern (triplanar takes its BLEND weights from the normal, its UV from position).
        float3 WorldPosition_DDX = InterpWeights(S.Bary.DDX, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float3 WorldPosition_DDY = InterpWeights(S.Bary.DDY, FinalWorldPos[0], FinalWorldPos[1], FinalWorldPos[2]);
        float4 VertexColor_DDX   = InterpWeights4(S.Bary.DDX, W[0].Color, W[1].Color, W[2].Color);
        float4 VertexColor_DDY   = InterpWeights4(S.Bary.DDY, W[0].Color, W[1].Color, W[2].Color);

        // Deferred texturing: sample with EXPLICIT gradients. There is no 2x2 quad here at all, so an
        // implicit derivative is not merely wrong across silhouettes -- it does not exist.
        //
        // The gradients are per-UV-chain, not UV0's: the material compiler propagates analytic derivatives
        // through the graph and passes each sample its own pair. Reusing UV0's for a DERIVED UV is a real
        // bug -- a 32x tiled UV then picks a mip 5 levels too fine (1024x the texel working set). Chains
        // with no derivable derivative fall back to UV0's, marked with a UV-GRADIENT FALLBACK comment.
        //
        // Scoped to the material graph only; the forward/masked raster passes keep implicit derivatives,
        // which are correct there.
#define SampleTexture2DAuto(TexIndex, SamplerIndex, UV, DDX, DDY) SampleTexture2DGrad((TexIndex), (SamplerIndex), (UV), (DDX), (DDY))
#define SampleTexture2DArrayAuto(TexIndex, SamplerIndex, UV, Slice, DDX, DDY) SampleTexture2DArrayGrad((TexIndex), (SamplerIndex), (UV), (Slice), (DDX), (DDY))
        $MATERIAL_INPUTS
#undef SampleTexture2DArrayAuto
#undef SampleTexture2DAuto

        G = BuildGBufferData(Input, Material);
    }

    const FGBufferPacked P = PackGBuffer(G);
    gRWTextures2D[Pass().GBufferAUAV][int2(Texel)] = P.A;
    gRWTextures2D[Pass().GBufferBUAV][int2(Texel)] = P.B;
    gRWTextures2D[Pass().GBufferCUAV][int2(Texel)] = P.C;
    gRWTextures2D[Pass().GBufferDUAV][int2(Texel)] = P.D;
}
