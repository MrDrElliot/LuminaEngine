#pragma once
#include "SceneGlobals.slang"
#include "AppendBuffer.slang"   // kAppendFull, the "no slot" sentinel MeshletSlotForWorkgroup returns
#include "Wind.slang"   // material-graph WindAnimation node; every material vertex template needs it

// Shared by every stage that touches a meshlet. What stays per-shader is the $MATERIAL_VERTEX_INPUTS
// token and the pass-specific final position.

struct FMeshletRef
{
    FMeshlet        M;
    uint            InstanceID;
    uint            GlobalMeshletIdx;
    // Surface and shadow LODs get separate compacted slices, so the base is picked alongside the
    // meshlet range rather than at load time.
    uint            SkinnedVertexBase;

    // The instance is REACHED, not stored: an FGPUInstance by value puts its MeshletHeader pointer in a
    // returned struct, which Slang materializes without the Aliased/Restrict SPIR-V requires there.
    property FGPUInstance* Inst { get { return Instances() + InstanceID; } }
};

// bShadow selects the instance's shadow meshlet range instead of the surface (base/depth) range.
FMeshletRef ResolveMeshlet(uint MeshletSlot, bool bShadow)
{
    // A pass that draws without these page-faults the device rather than producing a wrong pixel.
    if (MeshletDrawList() == nullptr || Instances() == nullptr)
    {
        FMeshletRef Empty;
        Empty.SkinnedVertexBase = kNoPreSkinBase;
        Empty.GlobalMeshletIdx  = 0u;
        Empty.InstanceID        = 0u;
        Empty.M                 = (FMeshlet)0;
        return Empty;
    }

    FMeshletDraw Draw = MeshletDrawList()[MeshletSlot];

    // InstanceID first: every Ref.Inst read below goes through it.
    FMeshletRef Ref;
    Ref.InstanceID = Draw.InstanceID;

    // A slot this frame never wrote resolves to a plausible instance from an earlier one. Unreachable
    // now, kept so that if that changes the result is missing geometry rather than wrong geometry.
    if (!Draw.IsFromFrame(Scene().CullData.MeshletDrawTag))
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    // bShadow selects only the pre-skin block: the draw entry already names the meshlet outright,
    // because the cull picks the LOD per view and the raster cannot know which one that was.
    Ref.SkinnedVertexBase    = bShadow ? Ref.Inst.ShadowSkinnedVertexBase : Ref.Inst.SkinnedVertexBase;

    // The tag says the slot was written this frame, not that the index inside it is in range. Unbounded,
    // these counts reach SetMeshOutputCounts and the output writes -- an out-of-bounds WRITE.
    //
    // Against the HEADER's count as well as the instance's: the arrays indexed below belong to the header
    // and only it knows their length. An instance naming a mesh that has died gets the null header, whose
    // count is 0, and takes this exit -- which is what replaced the null check above.
    if (Draw.MeshletIndex >= min(Ref.Inst.MeshletTotalCount, Ref.Inst.MeshletHeader.MeshletCount))
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    Ref.GlobalMeshletIdx     = Draw.MeshletIndex;
    Ref.M                    = Ref.Inst.MeshletHeader.Meshlets[Ref.GlobalMeshletIdx];

    // Where garbage stops propagating, not a clamp on legitimate data: past here the counts must be
    // within the DECLARED array sizes. Dropping the meshlet makes an overrun a hole, not a fault.
    if (Ref.M.VertexCount > MESHLET_MAX_VERTICES || Ref.M.TriangleCount > MESHLET_MAX_TRIANGLES)
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    return Ref;
}

FMeshletRef ResolveSurfaceMeshlet(uint MeshletSlot) { return ResolveMeshlet(MeshletSlot, false); }
FMeshletRef ResolveShadowMeshlet(uint MeshletSlot)  { return ResolveMeshlet(MeshletSlot, true);  }

// Declared here because BOTH stages must agree byte for byte and nothing checks it: a payload is not
// a linked interface, and task+mesh share one push range. Drift produces wrong geometry, not an error.
struct FMeshletPassArgs
{
    FRenderBucket*          Buckets;           // per-(view, draw) bases, bounds and slices

    uint                    ArgBase;           // CullViewIndex*NumDrawsPerView + IndirectDrawOffset
    uint                    Slice;             // MESHLET_SLICE_* of the bucket this pass rasterizes
    uint                    MaxMeshGroups;     // sub-draw d covers meshlets [d*MaxMeshGroups, (d+1)*...)
    uint                    CullViewIndex;
    int                     ShadowDataIndex;   // raster, shadow variants only
    int                     ViewIndex;         // raster, shadow variants only (cascade / face / spot)
    // For a shadow pass this is the atlas TILE, not the atlas. Two scalars so the struct needs no
    // interior padding under either block layout; the C++ push mirror matches.
    float                   ViewportW;
    float                   ViewportH;
};
FMeshletPassArgs* MeshletPass() { return GetPassArgs<FMeshletPassArgs>(); }

// The draw-list slot this mesh workgroup rasterizes, or kAppendFull past the slice. MeshletCull.slang
// already compacted the survivors, so a workgroup maps to one directly -- no payload, no second cull.
uint MeshletSlotForWorkgroup(uint DrawIndex, uint GroupIndex)
{
    const FRenderBucket Bucket = MeshletPass().Buckets[MeshletPass().ArgBase];
    const uint Slice  = MeshletPass().Slice;
    const uint Local  = DrawIndex * MeshletPass().MaxMeshGroups + GroupIndex;

    // The grid is sized from the same slice count, so this only rejects the tail of a partial sub-draw.
    if (Local >= Bucket.SliceCount[Slice])
    {
        return kAppendFull;
    }
    return Bucket.DrawBase + Bucket.SliceBase[Slice] + Local;
}

// The meshlet this workgroup rasterizes, plus the slot the VisBuffer packs into VisID. Zeroed
// (VertexCount == 0) past the slice or on an unresolvable entry, which every caller already treats as
// "emit nothing". Shadow-vs-surface comes from the CULL VIEW, exactly as the cull resolved it.
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

    const FCullView View = CullViews()[MeshletPass().CullViewIndex];
    return ResolveMeshlet(OutSlot, View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY));
}


// SPEC_SKINNED dead-strips the unused vertex-load path for homogeneous batches: 0=static-only, 1=skinned-only,
// 2=dynamic (per-instance runtime branch, used by mixed batches and by passes that don't specialize, e.g. deferred).
[vk::constant_id(5)] const uint SPEC_SKINNED = 2;

FVertexData LoadMeshletVertexData(FMeshletRef Ref, uint LocalVertIdx)
{
    bool bSkinned;
    if      (SPEC_SKINNED == 0u) { bSkinned = false; }
    else if (SPEC_SKINNED == 1u) { bSkinned = true;  }
    else                         { bSkinned = Ref.Inst.HasFlag(EInstanceFlags::Skinned); }

    if (bSkinned)
    {
        // Overflow path: entities the pre-skin budget could not cover blend per pass instead of once.
        if (Ref.SkinnedVertexBase == kNoPreSkinBase)
        {
            return DecodePreSkinnedVertex(SkinVertex(Ref.Inst.MeshletHeader, Ref.M, LocalVertIdx, Ref.Inst.BoneOffset));
        }
        return LoadSkinnedVertex(Ref.M, LocalVertIdx, Ref.SkinnedVertexBase);
    }
    return LoadStaticVertex(Ref.Inst.MeshletHeader, Ref.M, LocalVertIdx);
}

// One packed dword = three 8-bit local vertex indices. unpack_u8u32, not shift+mask: the hand-rolled
// form measured 9.97% of shader-profiler samples in the VisBuffer pass. This is one instruction.
uint3 UnpackMeshletTriangle(FMeshletRef Ref, uint TriIdx)
{
    uint Packed = Ref.Inst.MeshletHeader.Triangles[Ref.M.TriangleOffset + TriIdx];
    return unpack_u8u32(Packed).xyz;
}

// Every emitted index must address a vertex the workgroup DECLARED and WROTE, and a local index is
// unbounded imported data. Collapsing to (0,0,0) is the in-bounds way to skip a primitive.
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

// VS-emulation single-corner decode: corner in [0,3) of triangle TriIdx -> local vertex index.
uint MeshletCornerVertexIndex(FMeshletRef Ref, uint TriIdx, uint Corner)
{
    uint Packed = Ref.Inst.MeshletHeader.Triangles[Ref.M.TriangleOffset + TriIdx];
    return (Packed >> (Corner * 8u)) & 0xFFu;
}

// A spec constant, not a uniform: both answers are per-PIPELINE. BACKFACE must mirror the hardware
// cull mode -- CmdSetCullMode is DYNAMIC state the shader cannot observe. SMALLPRIM is off under MSAA.
[vk::constant_id(7)] const uint SPEC_TRI_CULL = 0;

static const uint TRI_CULL_BACKFACE  = 1u;
static const uint TRI_CULL_SMALLPRIM = 2u;

// Narrower than the meshlet on purpose: at 32 threads over a 64/64 meshlet each lane owns two
// vertices and two triangles, and the independent chains overlap instead of stalling one load.
#define MESHLET_MESH_GROUP_SIZE 32
#define MESHLET_VERT_ITERS ((MESHLET_MAX_VERTICES  + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)
#define MESHLET_TRI_ITERS  ((MESHLET_MAX_TRIANGLES + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)

// Out of the OWNING LANE'S REGISTERS. Correct only because the workgroup is exactly one subgroup --
// WaveReadLaneAt cannot cross waves. Every lane must be active, so both loops in the mesh shaders run
// their LOAD/TEST on a clamped index. Their EXPORT index is the unclamped ThreadID + iter*GroupSize
// instead, guarded by a bounds test: an export index the compiler cannot prove equals the lane forces
// the whole export through LDS. Clip[] is therefore written by every lane, clamped ones included -- it
// is read by LANE, and for any V < VertexCount the owning lane holds the unclamped value.
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

// Operates in CLIP space so a vertex behind the eye is handled explicitly rather than divided by a
// negative w, which reads as a winding flip and would cull front faces straddling the near plane.
bool IsMeshletTriangleCulled(float4 C0, float4 C1, float4 C2, float2 ViewportSize)
{
    // Three vertices outside the SAME plane means no intersection; three DIFFERENT planes proves nothing.
    // Non-short-circuiting on purpose: all() over a bool3 is arithmetic, not ~15 branches.
    const bool bOutside = all(bool3(C0.x >  C0.w, C1.x >  C1.w, C2.x >  C2.w))
                        | all(bool3(C0.x < -C0.w, C1.x < -C1.w, C2.x < -C2.w))
                        | all(bool3(C0.y >  C0.w, C1.y >  C1.w, C2.y >  C2.w))
                        | all(bool3(C0.y < -C0.w, C1.y < -C1.w, C2.y < -C2.w))
                        | all(bool3(C0.z <  0.0,  C1.z <  0.0,  C2.z <  0.0));
    if (bOutside)
    {
        return true;
    }

    // The perspective divide is meaningless here and every test below lives in NDC. The hardware clipper
    // splits such a triangle correctly, so keep it.
    if (any(bool3(C0.w <= 0.0, C1.w <= 0.0, C2.w <= 0.0)))
    {
        return false;
    }

    // Twice the signed area as the homogeneous 3x3 determinant: it equals Area * w0*w1*w2 and every w is
    // positive here, so the SIGN matches NDC without a divide. Front faces are counter-clockwise.
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
        // The rasterizer samples at pixel CENTERS, so the triangle contributes nothing unless some integer i
        // satisfies Lo <= i + 0.5 <= Hi on both axes. Testing the bounding box keeps it conservative.
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

// Zero area whatever the transform does, so it never needs its vertices resolved. Also catches the
// (0,0,0) UnpackMeshletTriangleSafe collapses an out-of-range triple to.
bool IsDegenerateTriangleIndices(uint3 Idx)
{
    return Idx.x == Idx.y || Idx.y == Idx.z || Idx.z == Idx.x;
}

struct FMeshletWorldVertex
{
    float3 WorldPos;     // pre-WorldPositionOffset
    float3 NormalWS;
    float4 TangentWS;    // xyz: world tangent, w: MikkTSpace handedness
    float2 UV;
    float2 UV1;
    float4 Color;
};

// BY VALUE so the caller can hoist the loads: reaching ModelMatrix through Ref.Inst is a dependent
// chain the compiler cannot overlap, since a PhysicalStorageBuffer pointer may alias anything.
FMeshletWorldVertex TransformMeshletVertex(FVertexData V, float4x4 ModelMatrix, float3x3 NormalMatrixWS)
{
    FMeshletWorldVertex W;
    W.WorldPos  = mul(ModelMatrix, float4(V.Position, 1.0)).xyz;
    W.NormalWS  = mul(NormalMatrixWS, V.Normal);
    // Tangents take the forward 3x3 (they lie in the surface); re-orthogonalized per-pixel.
    W.TangentWS = float4(mul((float3x3)ModelMatrix, V.Tangent.xyz), V.Tangent.w);
    W.UV        = V.UV;
    W.UV1       = V.UV1;
    W.Color     = V.Color;
    return W;
}

FMeshletWorldVertex TransformMeshletVertex(FMeshletRef Ref, FVertexData V, float3x3 NormalMatrixWS)
{
    return TransformMeshletVertex(V, Ref.Inst.ModelMatrix, NormalMatrixWS);
}

FMeshletWorldVertex TransformMeshletVertex(FMeshletRef Ref, FVertexData V)
{
    const float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
    return TransformMeshletVertex(V, ModelMatrix, CalculateNormalMatrix(ModelMatrix));
}

// Base pass and depth pre-pass MUST derive SV_Position from this exact pair, in this order, or the
// [earlydepthstencil] equal-depth test breaks. Do not fuse them into a view-projection product.
float4 CameraViewPosition(float3 WorldPos, float4x4 CameraView) { return mul(CameraView, float4(WorldPos, 1.0)); }
float4 CameraClipPosition(float4 ViewPos, float4x4 CameraProj)  { return mul(CameraProj, ViewPos); }

float4 CameraViewPosition(float3 WorldPos) { return CameraViewPosition(WorldPos, GetCameraView()); }
float4 CameraClipPosition(float4 ViewPos)  { return CameraClipPosition(ViewPos, GetCameraProjection()); }

// Produced by the VS emulation AND the mesh shader, consumed by BasePixelPass. Per-instance constants
// are forwarded flat so the PS never re-reads the 128B FGPUInstance per pixel.
struct FBaseVertexOutput
{
    float4 FragColor                   : COLOR;
    float4 NormalVS                    : NORMAL_VS;
    float4 NormalWS                    : NORMAL_WS;
    float4 TangentWS                   : TANGENT_WS;
    float4 FragPos                     : FRAG_POS;
    float2 UV                          : TEXCOORD0;
    // Costs 8B of mesh-shader output storage per vertex on every base-pass draw, whether or not the
    // material samples it (see the occupancy note on FDepthOnlyVertexOutput below).
    float2 UV1                         : TEXCOORD1;
    uint   InstanceIndex               : INSTANCE;
    nointerpolation uint EntityID      : ENTITY_ID;
    nointerpolation uint MaterialIndex : MATERIAL_INDEX;
    nointerpolation uint InstanceFlags : INSTANCE_FLAGS;
    nointerpolation uint MeshletIndex  : MESHLET_INDEX;
    precise float4 Position            : SV_Position;
};

// Output storage is allocated from the DECLARED type at pipeline creation, and caps mesh-workgroup
// residency: FBaseVertexOutput for a depth-only pass reserved 124B to carry 16B, at 4.2% occupancy.
struct FDepthOnlyVertexOutput
{
    precise float4 Position : SV_Position;
};

// FinalWorldPos is post-WPO: the shader adds Material.WorldPositionOffset before calling this.
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
    O.UV            = W.UV;
    O.UV1           = W.UV1;
    O.InstanceIndex = Ref.InstanceID;
    O.EntityID      = Ref.Inst.EntityID;
    O.MaterialIndex = Ref.Inst.MaterialIndex;
    O.InstanceFlags = (uint)Ref.Inst.Flags;
    O.MeshletIndex  = Ref.GlobalMeshletIdx;
    O.Position      = CameraClipPosition(ViewPos, CameraProj);
    return O;
}

FBaseVertexOutput MakeBaseVertexOutput(FMeshletRef Ref, FMeshletWorldVertex W, float3 FinalWorldPos)
{
    return MakeBaseVertexOutput(Ref, W, FinalWorldPos, GetCameraView(), GetCameraProjection());
}
