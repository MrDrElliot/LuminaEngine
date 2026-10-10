#pragma once
#include "SceneGlobals.slang"
#include "AppendBuffer.slang"
#include "Wind.slang"   // For the material graph's WindAnimation node
#include "Triplanar.slang"   // For the material graph's TriplanarSample node

struct FMeshletRef
{
    FMeshlet        M;
    uint            InstanceID;
    uint            GlobalMeshletIdx;
    uint            SkinnedVertexBase;

    // Reached through a pointer since this struct is returned by value and the instance is 128 bytes.
    property FGPUInstance* Inst { get { return Instances() + InstanceID; } }

    // FGPUInstance::MeshletHeader would load the whole instance to reach the slot.
    property FMeshletHeader* Header { get { return MeshletHeaders().At(Inst.MeshletHeaderSlot); } }

    // Transform sits at offset 0 of a 16-aligned 128-byte stride, so 48 aligned bytes cover it.
    property float4x4 ModelMatrix { get { return loadAligned<16>(&Inst.Transform).ToMatrix(); } }

    property EInstanceFlags InstFlags { get { return FGPUInstance::GetFlags(Inst); } }
    bool InstHasFlag(EInstanceFlags Flag) { return FGPUInstance::HasFlag(Inst, Flag); }
};

FMeshletRef ResolveMeshlet(uint MeshletSlot)
{

    // Issued before the draw fetch so the two independent chains overlap instead of serializing.
    const uint ExpectedTag = Scene().CullData.MeshletDrawTag;

    FMeshletDraw Draw = MeshletDrawList().Get(MeshletSlot);

    FMeshletRef Ref;
    Ref.InstanceID = Draw.InstanceID;

    // A slot this frame never wrote drops to missing geometry rather than an earlier frame's instance.
    if (!Draw.IsFromFrame(ExpectedTag))
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    // The draw entry names the meshlet outright, since the cull picks the LOD per view and the raster cannot know which.
    Ref.SkinnedVertexBase    = Ref.Inst.SkinnedVertexBase;

    // Also bounded by the header count (0 for a dead mesh's null header), since these counts reach SetMeshOutputCounts.
    if (Draw.MeshletIndex >= min(Ref.Inst.MeshletTotalCount, Ref.Header.MeshletCount))
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    Ref.GlobalMeshletIdx     = Draw.MeshletIndex;
    Ref.M                    = loadAligned<16>(Ref.Header.Meshlets + Ref.GlobalMeshletIdx);

    // Counts past the declared array sizes are garbage, so drop the meshlet rather than overrun.
    if (Ref.M.VertexCount > MESHLET_MAX_VERTICES || Ref.M.TriangleCount > MESHLET_MAX_TRIANGLES)
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    return Ref;
}

// One view of a multi-view shadow draw, mirrored by FShadowRasterViewGPU.
struct FShadowRasterView
{
    int    ShadowDataIndex;
    int    ViewIndex;
    float2 TileSize;
};

// Task and mesh share this push range unchecked, so both stages must agree byte for byte.
struct FMeshletPassArgs
{
    GPUSpan<FRenderBucket>  Buckets;

    uint                    ArgBase;           // CullViewIndex*NumDrawsPerView + IndirectDrawOffset
    uint                    Slice;             // MESHLET_SLICE_* of the bucket this pass rasterizes
    uint                    MaxMeshGroups;     // sub-draw d covers meshlets [d*MaxMeshGroups, (d+1)*...)
    uint                    CullViewIndex;
    int                     ShadowDataIndex;   // Shadow raster variants only
    int                     ViewIndex;         // Shadow raster variants only (cascade, face or spot)
    // The atlas tile size for a shadow pass, as two scalars so neither block layout pads.
    float                   ViewportW;
    float                   ViewportH;

    // Set when one draw covers consecutive shadow cull views, and SV_DrawIndex then picks the view and its viewport.
    GPUSpan<FShadowRasterView> ShadowViews;
    // The bucket distance between consecutive cull views, which is NumDrawsPerView.
    uint                    ArgViewStride;
    uint                    _Pad0;
};
FMeshletPassArgs* MeshletPass() { return GetPassArgs<FMeshletPassArgs>(); }

bool IsMultiViewShadowDraw()
{
    return MeshletPass().ShadowViews.Count > 0u;
}

FShadowRasterView ShadowRasterViewForDraw(uint DrawIndex)
{
    if (IsMultiViewShadowDraw())
    {
        return MeshletPass().ShadowViews.Get(DrawIndex);
    }

    FShadowRasterView View;
    View.ShadowDataIndex = MeshletPass().ShadowDataIndex;
    View.ViewIndex       = MeshletPass().ViewIndex;
    View.TileSize        = float2(MeshletPass().ViewportW, MeshletPass().ViewportH);
    return View;
}

// Returns kAppendFull past the slice. MeshletCull already compacted survivors, so there is no payload or second cull.
uint MeshletSlotForWorkgroup(uint DrawIndex, uint GroupIndex)
{
    // A multi-view draw only runs when every slice fits one sub-draw, so its draw index walks views instead.
    const bool bMultiView = IsMultiViewShadowDraw();
    const uint ArgBase    = MeshletPass().ArgBase + (bMultiView ? DrawIndex * MeshletPass().ArgViewStride : 0u);
    const uint SubDraw    = bMultiView ? 0u : DrawIndex;

    const FRenderBucket Bucket = MeshletPass().Buckets.GetAligned(ArgBase);
    const uint Slice  = MeshletPass().Slice;
    const uint Local  = SubDraw * MeshletPass().MaxMeshGroups + GroupIndex;

    // The grid is sized from the same slice count, so this only rejects the tail of a partial sub-draw.
    if (Local >= Bucket.SliceCount[Slice])
    {
        return kAppendFull;
    }
    return Bucket.DrawBase + Bucket.SliceBase[Slice] + Local;
}

// Zeroed (VertexCount == 0) past the slice or on an unresolvable entry, which callers treat as emit nothing.
FMeshletRef ResolveMeshletForWorkgroup(uint DrawIndex, uint GroupIndex, out uint OutSlot)
{
    OutSlot = MeshletSlotForWorkgroup(DrawIndex, GroupIndex);
    if (OutSlot == kAppendFull)
    {
        FMeshletRef Empty;
        Empty.SkinnedVertexBase = kNoPreSkinBase;
        Empty.GlobalMeshletIdx  = 0u;
        Empty.InstanceID        = 0u;
        Empty.M                 = (FMeshlet)0;
        return Empty;
    }

    return ResolveMeshlet(OutSlot);
}


// 0 static-only, 1 skinned-only, 2 per-instance branch for mixed batches and unspecialized passes.
[vk::constant_id(5)] const uint SPEC_SKINNED = 2;

// bWholeVertex is a constant at every call site, so the unused static loader compiles out.
FVertexData LoadMeshletVertexData(FMeshletRef Ref, uint LocalVertIdx, bool bWholeVertex = false)
{
    bool bSkinned;
    if      (SPEC_SKINNED == 0u) { bSkinned = false; }
    else if (SPEC_SKINNED == 1u) { bSkinned = true;  }
    else                         { bSkinned = Ref.InstHasFlag(EInstanceFlags::Skinned); }

    if (bSkinned)
    {
        // Entities past the pre-skin budget blend per pass instead of once.
        if (Ref.SkinnedVertexBase == kNoPreSkinBase)
        {
            return DecodePreSkinnedVertex(SkinVertex(Ref.Header, Ref.M, Ref.GlobalMeshletIdx, LocalVertIdx, Ref.Inst.BoneOffset));
        }
        return LoadSkinnedVertex(Ref.M, LocalVertIdx, Ref.SkinnedVertexBase);
    }
    return bWholeVertex ? LoadStaticVertexWhole(Ref.Header, Ref.M, LocalVertIdx)
                        : LoadStaticVertex(Ref.Header, Ref.M, LocalVertIdx);
}

// unpack_u8u32 is one instruction, where hand-rolled shift and mask measured 10% of VisBuffer samples.
uint3 UnpackMeshletTriangle(FMeshletRef Ref, uint TriIdx)
{
    uint Packed = Ref.Header.Triangles[Ref.M.TriangleOffset + TriIdx];
    return unpack_u8u32(Packed).xyz;
}

// Collapsing to (0,0,0) skips a primitive whose unbounded local index the workgroup may never have written.
uint3 UnpackMeshletTriangleSafe(FMeshletRef Ref, uint TriIdx)
{
    const uint3 Tri = UnpackMeshletTriangle(Ref, TriIdx);
    const uint  VC  = Ref.M.VertexCount;
    if (Tri.x >= VC || Tri.y >= VC || Tri.z >= VC)
    {
        return uint3(0u, 0u, 0u);
    }
    return Tri;
}

uint MeshletCornerVertexIndex(FMeshletRef Ref, uint TriIdx, uint Corner)
{
    uint Packed = Ref.Header.Triangles[Ref.M.TriangleOffset + TriIdx];
    return (Packed >> (Corner * 8u)) & 0xFFu;
}

// Must mirror the pipeline cull mode, which is dynamic state the shader cannot observe. Small-primitive culling is off under MSAA.
[vk::constant_id(7)] const uint SPEC_TRI_CULL = 0;

static const uint TRI_CULL_BACKFACE  = 1u;
static const uint TRI_CULL_SMALLPRIM = 2u;

// 32 threads over a 64/64 meshlet gives each lane two independent load chains that overlap.
#define MESHLET_MESH_GROUP_SIZE 32
#define MESHLET_VERT_ITERS ((MESHLET_MAX_VERTICES  + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)
#define MESHLET_TRI_ITERS  ((MESHLET_MAX_TRIANGLES + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)

// Needs the workgroup to be exactly one subgroup with every lane active, so callers fill Clip[] on clamped lanes too.
float4 ShuffleMeshletClip(float4 Clip[MESHLET_VERT_ITERS], uint V)
{
    const uint Lane = V % MESHLET_MESH_GROUP_SIZE;

    float4 Result = WaveReadLaneAt(Clip[0], Lane);

    [ForceUnroll]
    for (uint i = 1; i < MESHLET_VERT_ITERS; ++i)
    {
        const float4 Candidate = WaveReadLaneAt(Clip[i], Lane);
        Result = (V / MESHLET_MESH_GROUP_SIZE) == i ? Candidate : Result;
    }
    return Result;
}

// Clip space so a vertex behind the eye is not divided by a negative w and misread as a winding flip.
bool IsMeshletTriangleCulled(float4 C0, float4 C1, float4 C2, float2 ViewportSize)
{
    // All three outside one plane culls. The bitwise | keeps it branch-free.
    const bool bOutside = all(bool3(C0.x >  C0.w, C1.x >  C1.w, C2.x >  C2.w))
                        | all(bool3(C0.x < -C0.w, C1.x < -C1.w, C2.x < -C2.w))
                        | all(bool3(C0.y >  C0.w, C1.y >  C1.w, C2.y >  C2.w))
                        | all(bool3(C0.y < -C0.w, C1.y < -C1.w, C2.y < -C2.w))
                        | all(bool3(C0.z <  0.0,  C1.z <  0.0,  C2.z <  0.0));
    if (bOutside)
    {
        return true;
    }

    // The hardware clipper splits a triangle crossing w = 0, so keep it.
    if (any(bool3(C0.w <= 0.0, C1.w <= 0.0, C2.w <= 0.0)))
    {
        return false;
    }

    // The homogeneous determinant is area times w0*w1*w2, so its sign matches NDC. Front faces are counter-clockwise.
    const float Det = C0.x * (C1.y * C2.w - C1.w * C2.y)
                    - C0.y * (C1.x * C2.w - C1.w * C2.x)
                    + C0.w * (C1.x * C2.y - C1.y * C2.x);

    if ((SPEC_TRI_CULL & TRI_CULL_BACKFACE) != 0u)
    {
        // Spelled !(Det > 0) rather than (Det <= 0) so a NaN lands in the reject too.
        if (!(Det > 0.0))
        {
            return true;
        }
    }
    else if (!(abs(Det) > 0.0))
    {
        // Two-sided keeps both windings, but a zero-area or NaN triangle still rasterizes nothing.
        return true;
    }

    if ((SPEC_TRI_CULL & TRI_CULL_SMALLPRIM) != 0u && all(ViewportSize > 0.0))
    {
        // The rasterizer samples pixel centers, so the box must straddle some i + 0.5 on both axes.
        const float2 P0 = C0.xy / C0.w;
        const float2 P1 = C1.xy / C1.w;
        const float2 P2 = C2.xy / C2.w;

        const float2 Scale = ViewportSize * 0.5;
        const float2 Q0    = P0 * Scale + Scale;
        const float2 Q1    = P1 * Scale + Scale;
        const float2 Q2    = P2 * Scale + Scale;

        const float2 Lo = min(min(Q0, Q1), Q2) - 0.5;
        const float2 Hi = max(max(Q0, Q1), Q2) - 0.5;

        if (any(ceil(Lo) > floor(Hi)))
        {
            return true;
        }
    }

    return false;
}

// Also catches the (0,0,0) collapse from UnpackMeshletTriangleSafe.
bool IsDegenerateTriangleIndices(uint3 Idx)
{
    return Idx.x == Idx.y || Idx.y == Idx.z || Idx.z == Idx.x;
}

struct FMeshletWorldVertex
{
    float3 WorldPos;     // Before WorldPositionOffset
    float3 NormalWS;
    float4 TangentWS;    // w is the MikkTSpace handedness
    float2 UV;
    float2 UV1;
    float4 Color;
};

// By value so callers can hoist the loads, since a PhysicalStorageBuffer pointer may alias anything.
FMeshletWorldVertex TransformMeshletVertex(FVertexData V, float4x4 ModelMatrix, float3x3 NormalMatrixWS)
{
    FMeshletWorldVertex W;
    W.WorldPos  = mul(ModelMatrix, float4(V.Position, 1.0)).xyz;
    W.NormalWS  = mul(NormalMatrixWS, V.Normal);
    // Tangents lie in the surface, so they take the forward 3x3 and are re-orthogonalized per pixel.
    W.TangentWS = float4(mul((float3x3)ModelMatrix, V.Tangent.xyz), V.Tangent.w);
    W.UV        = V.UV;
    W.UV1       = V.UV1;
    W.Color     = V.Color;
    return W;
}

FMeshletWorldVertex TransformMeshletVertex(FMeshletRef Ref, FVertexData V, float3x3 NormalMatrixWS)
{
    return TransformMeshletVertex(V, Ref.ModelMatrix, NormalMatrixWS);
}

FMeshletWorldVertex TransformMeshletVertex(FMeshletRef Ref, FVertexData V)
{
    const float4x4 ModelMatrix = Ref.ModelMatrix;
    return TransformMeshletVertex(V, ModelMatrix, CalculateNormalMatrix(ModelMatrix));
}

// Base pass and depth prepass must build SV_Position from this exact pair or the equal-depth test breaks.
float4 CameraViewPosition(float3 WorldPos, float4x4 CameraView) { return mul(CameraView, float4(WorldPos, 1.0)); }
float4 CameraClipPosition(float4 ViewPos, float4x4 CameraProj)  { return mul(CameraProj, ViewPos); }

float4 CameraViewPosition(float3 WorldPos) { return CameraViewPosition(WorldPos, GetCameraView()); }
float4 CameraClipPosition(float4 ViewPos)  { return CameraClipPosition(ViewPos, GetCameraProjection()); }

// Per-instance constants are forwarded flat so the PS never rereads the 128B FGPUInstance.
struct FBaseVertexOutput
{
    float4 FragColor                   : COLOR;
    float4 NormalVS                    : NORMAL_VS;
    float4 NormalWS                    : NORMAL_WS;
    float4 TangentWS                   : TANGENT_WS;
    float4 FragPos                     : FRAG_POS;
    // Pre-WPO position for the graph's ExcludingOffsets reads.
    float3 WorldPosNoWPO                : WORLD_POS_NO_WPO;
    float2 UV                          : TEXCOORD0;
    float2 UV1                         : TEXCOORD1;
    uint   InstanceIndex               : INSTANCE;
    nointerpolation uint EntityID      : ENTITY_ID;
    nointerpolation uint MaterialIndex : MATERIAL_INDEX;
    nointerpolation uint InstanceFlags : INSTANCE_FLAGS;
    nointerpolation uint MeshletIndex  : MESHLET_INDEX;
    precise float4 Position            : SV_Position;
};

// Mesh output storage is sized from the declared type and caps workgroup residency, so depth-only carries only Position.
struct FDepthOnlyVertexOutput
{
    precise float4 Position : SV_Position;
};

// Masked lanes carry only what the pixel graph names, since output storage caps mesh workgroup residency.
#ifdef MASKED_IO_FULL
typedef FBaseVertexOutput FMaskedVertexOutput;
#else
struct FMaskedVertexOutput
{
    float2 UV                          : TEXCOORD0;
#ifdef MASKED_IO_UV1
    float2 UV1                         : TEXCOORD1;
#endif
#ifdef MASKED_IO_COLOR
    float4 FragColor                   : COLOR;
#endif
#ifdef MASKED_IO_NORMAL
    float3 NormalWS                    : NORMAL_WS;
#endif
#ifdef MASKED_IO_TANGENT
    float4 TangentWS                   : TANGENT_WS;
#endif
#ifdef MASKED_IO_VIEW_POS
    float3 FragPos                     : FRAG_POS;
#endif
#ifdef MASKED_IO_WORLD_NO_WPO
    float3 WorldPosNoWPO               : WORLD_POS_NO_WPO;
#endif
#ifdef MASKED_IO_INSTANCE
    nointerpolation uint InstanceIndex : INSTANCE;
    nointerpolation uint EntityID      : ENTITY_ID;
    nointerpolation uint InstanceFlags : INSTANCE_FLAGS;
#endif
    nointerpolation uint MaterialIndex : MATERIAL_INDEX;
    precise float4 Position            : SV_Position;
};
#endif

// FinalWorldPos already includes Material.WorldPositionOffset.
FBaseVertexOutput MakeBaseVertexOutput(FMeshletRef Ref, FMeshletWorldVertex W, float3 FinalWorldPos,
                                       float4x4 CameraView, float4x4 CameraProj)
{
    float4 ViewPos = CameraViewPosition(FinalWorldPos, CameraView);

    FBaseVertexOutput O;
    O.FragColor     = W.Color;
    O.NormalVS      = float4(normalize(NormalWorldToView(W.NormalWS)), 1.0);
    O.NormalWS      = float4(W.NormalWS, 1.0);
    O.TangentWS     = W.TangentWS;
    O.FragPos       = ViewPos;
    O.WorldPosNoWPO = W.WorldPos;
    O.UV            = W.UV;
    O.UV1           = W.UV1;
    O.InstanceIndex = Ref.InstanceID;
    O.EntityID      = Ref.Inst.EntityID;
    O.MaterialIndex = Ref.Inst.MaterialIndex;
    O.InstanceFlags = (uint)Ref.InstFlags;
    O.MeshletIndex  = Ref.GlobalMeshletIdx;
    O.Position      = CameraClipPosition(ViewPos, CameraProj);
    return O;
}

FBaseVertexOutput MakeBaseVertexOutput(FMeshletRef Ref, FMeshletWorldVertex W, float3 FinalWorldPos)
{
    return MakeBaseVertexOutput(Ref, W, FinalWorldPos, GetCameraView(), GetCameraProjection());
}

// MaterialIndex is passed in so the workgroup loads it once, ahead of the vertex phase.
FMaskedVertexOutput MakeMaskedVertexOutput(FMeshletRef Ref, FMeshletWorldVertex W, float3 FinalWorldPos,
                                           float4x4 CameraView, float4x4 CameraProj, uint MaterialIndex)
{
#ifdef MASKED_IO_FULL
    return MakeBaseVertexOutput(Ref, W, FinalWorldPos, CameraView, CameraProj);
#else
    const float4 ViewPos = CameraViewPosition(FinalWorldPos, CameraView);

    FMaskedVertexOutput O;
    O.UV            = W.UV;
#ifdef MASKED_IO_UV1
    O.UV1           = W.UV1;
#endif
#ifdef MASKED_IO_COLOR
    O.FragColor     = W.Color;
#endif
#ifdef MASKED_IO_NORMAL
    O.NormalWS      = W.NormalWS;
#endif
#ifdef MASKED_IO_TANGENT
    O.TangentWS     = W.TangentWS;
#endif
#ifdef MASKED_IO_VIEW_POS
    O.FragPos       = ViewPos.xyz;
#endif
#ifdef MASKED_IO_WORLD_NO_WPO
    O.WorldPosNoWPO = W.WorldPos;
#endif
#ifdef MASKED_IO_INSTANCE
    O.InstanceIndex = Ref.InstanceID;
    O.EntityID      = Ref.Inst.EntityID;
    O.InstanceFlags = (uint)Ref.InstFlags;
#endif
    O.MaterialIndex = MaterialIndex;
    O.Position      = CameraClipPosition(ViewPos, CameraProj);
    return O;
#endif
}

// Pixel-side reads of the masked interpolants. A field the graph never names reads as a neutral value the compiler strips.
float2 MaskedUV1(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_UV1
    return Input.UV1;
#else
    return float2(0.0, 0.0);
#endif
}

float4 MaskedVertexColor(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_COLOR
    return Input.FragColor;
#else
    return float4(1.0, 1.0, 1.0, 1.0);
#endif
}

float3 MaskedWorldNormal(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_NORMAL
    return normalize(Input.NormalWS.xyz);
#else
    return float3(0.0, 1.0, 0.0);
#endif
}

float4 MaskedWorldTangent(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_TANGENT
    return Input.TangentWS;
#else
    return float4(1.0, 0.0, 0.0, 1.0);
#endif
}

float3 MaskedWorldPosNoWPO(FMaskedVertexOutput Input, float3 WorldPosition)
{
#ifdef MASKED_IO_WORLD_NO_WPO
    return Input.WorldPosNoWPO;
#else
    return WorldPosition;
#endif
}

uint MaskedEntityID(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_INSTANCE
    return Input.EntityID;
#else
    return 0u;
#endif
}

uint MaskedInstanceFlags(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_INSTANCE
    return Input.InstanceFlags;
#else
    return 0u;
#endif
}

// Camera-pass view position. Without the interpolant it is unprojected from the fragment, which is the same point.
float3 MaskedCameraViewPosition(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_VIEW_POS
    return Input.FragPos.xyz;
#else
    return ReconstructViewPos(Input.Position.xy / float2(GetScreenSize()), Input.Position.z, GetInverseCameraProjection());
#endif
}

// Shadow-pass view position. Its fragment is in light space, so only the interpolant can give the camera-space point.
float3 MaskedShadowViewPosition(FMaskedVertexOutput Input)
{
#ifdef MASKED_IO_VIEW_POS
    return Input.FragPos.xyz;
#else
    return float3(0.0, 0.0, 0.0);
#endif
}
