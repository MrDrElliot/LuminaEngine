#pragma once
#include "SceneGlobals.slang"
#include "Wind.slang"   // material-graph WindAnimation node; every material vertex template needs it

// Shared by every stage that touches a meshlet: the amplification stage, the four mesh variants and the
// deferred reconstruction. What stays per-shader is the $MATERIAL_VERTEX_INPUTS token and the
// pass-specific final position.

struct FMeshletRef
{
    FMeshlet        M;
    uint            InstanceID;
    uint            GlobalMeshletIdx;
    // Surface and shadow LODs get separate compacted slices, so the base is picked alongside the
    // meshlet range rather than at load time.
    uint            SkinnedVertexBase;

    // The instance is REACHED, not stored: an FGPUInstance by value would put its MeshletHeader
    // pointer inside a returned struct, which Slang materializes as a Function OpVariable without the
    // Aliased or Restrict SPIR-V requires there. Keeping only the index leaves FMeshletRef
    // pointer-free, which is what makes the return legal.
    //
    // Only valid on a ref that resolved. The failure paths below all zero M, and every caller tests M.
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

    // A slot this frame never wrote resolves to a plausible-looking instance from an earlier frame.
    // Unreachable now the raster only draws slots the cull filled, and kept so that if that stops being
    // true the result is missing geometry rather than the wrong geometry.
    if (!Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) || Ref.Inst.MeshletHeader == nullptr)
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    // bShadow selects only the pre-skin block: the draw entry already names the meshlet outright,
    // because the cull picks the LOD per view and the raster cannot know which one that was.
    Ref.SkinnedVertexBase    = bShadow ? Ref.Inst.ShadowSkinnedVertexBase : Ref.Inst.SkinnedVertexBase;

    // The tag says the slot was written this frame; it says nothing about the index inside it. Unbounded,
    // this reads whatever follows the meshlet array, and those counts then drive SetMeshOutputCounts and
    // the output array writes -- which is how a bad index here becomes an out-of-bounds WRITE.
    if (Draw.MeshletIndex >= Ref.Inst.MeshletTotalCount)
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    Ref.GlobalMeshletIdx     = Draw.MeshletIndex;
    Ref.M                    = Ref.Inst.MeshletHeader.Meshlets[Ref.GlobalMeshletIdx];

    // Where garbage stops propagating, not a clamp on legitimate data: past here the counts reach
    // SetMeshOutputCounts, which the spec requires to be within the DECLARED array sizes. Dropping the
    // meshlet makes an overrun a visible hole rather than an undefined interpolant and a fault.
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

// The payload and push block below are declared here because BOTH stages have to agree on them byte for
// byte and nothing checks that they do: a payload is not an interface SPIR-V links, and a task and mesh
// stage in the same pipeline share one push-constant range. A drifted layout produces geometry from the
// wrong meshlet, not a compile error.
struct FMeshletPassArgs
{
    FRenderBucket*          Buckets;           // per-(view, draw) bases, bounds and cursors
    FMeshletBlock*          BlockList;
    // One word per BLOCK, indexed exactly as BlockList is. Written by the early phase, read by the late
    // one. A block is MESHLET_TASK_GROUP_SIZE == 32 meshlets, so a block's defer set is one uint of lane
    // bits and needs no allocation, no cursor and no capacity of its own -- which is the whole reason the
    // late phase can re-dispatch the early phase's grid verbatim instead of building a compacted list.
    uint*                   DeferBits;

    uint                    ArgBase;           // CullViewIndex*NumDrawsPerView + IndirectDrawOffset
    uint                    MaxTaskGroups;     // sub-draw d covers blocks [d*MaxTaskGroups, (d+1)*...)
    uint                    CullViewIndex;
    uint                    CullPhase;         // CULL_PHASE_EARLY / CULL_PHASE_LATE
    int                     ShadowDataIndex;   // raster, shadow variants only
    int                     ViewIndex;         // raster, shadow variants only (cascade / face / spot)
    // For a shadow pass this is the atlas TILE, not the atlas. Two scalars so the struct needs no
    // interior padding under either block layout; the C++ push mirror matches.
    float                   ViewportW;
    float                   ViewportH;
};
FMeshletPassArgs* MeshletPass() { return GetPassArgs<FMeshletPassArgs>(); }

// One resolved meshlet, 36 bytes. No ModelMatrix: carrying it made the payload 3584 B, and on RDNA the
// payload spills to a per-draw VRAM ring whose capacity bounds task-workgroup occupancy.
struct FTaskMeshletEntry
{
    FMeshlet M;                   // amortizes the deepest dependent hop (Header -> Meshlets[idx])
    uint     MeshletSlot;         // draw-list slot; the mesh shader packs it into VisID
    uint     GlobalMeshletIdx;    // index into the mesh's meshlet array, for MakeBaseVertexOutput
};

// 16 + 36*32 = 1168 B. InstanceID and SkinnedVertexBase are workgroup-scope: one block is one instance,
// so all 32 entries would otherwise carry the same two values.
struct FVisTaskPayload
{
    uint InstanceID;
    uint SkinnedVertexBase;
    uint _Pad0;
    uint _Pad1;
    FTaskMeshletEntry Entries[MESHLET_TASK_GROUP_SIZE];
};

// Rebuilds the ref the task stage already paid for. Every bound check ResolveMeshlet makes was already
// made by the task lane that filled this entry: an entry that reached the payload resolved cleanly.
FMeshletRef MakeMeshletRefFromTask(FVisTaskPayload Payload, uint Index)
{
    FMeshletRef Ref;
    Ref.M                 = Payload.Entries[Index].M;
    Ref.GlobalMeshletIdx  = Payload.Entries[Index].GlobalMeshletIdx;
    Ref.InstanceID        = Payload.InstanceID;
    Ref.SkinnedVertexBase = Payload.SkinnedVertexBase;
    return Ref;
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

// One packed dword = three 8-bit local vertex indices (the meshlet's triangle).
//
// unpack_u8u32, not three shift+mask pairs: the hand-rolled form measured 9.97% of shader-profiler samples
// in the VisBuffer pass, second only to the dependent load feeding it. This is one instruction.
uint3 UnpackMeshletTriangle(FMeshletRef Ref, uint TriIdx)
{
    uint Packed = Ref.Inst.MeshletHeader.Triangles[Ref.M.TriangleOffset + TriIdx];
    return unpack_u8u32(Packed).xyz;
}

// Every emitted index must address a vertex the workgroup both DECLARED and WROTE, and a local index is
// eight bits of imported data nothing bounds against this meshlet's vertex count. Collapsing to (0,0,0)
// is the in-bounds way to spell "skip this primitive": zero area, and vertex 0 is always written.
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

// Per-triangle culling, mesh path only: the VS-emulation path draws a fixed 3-vertices-per-triangle
// range and has no way to not emit one.
//
// A spec constant, not a uniform, because both answers are per-PIPELINE and a disabled test must cost
// nothing. BACKFACE must mirror the pipeline's hardware cull mode: CmdSetCullMode is DYNAMIC state the
// shader cannot observe, so a two-sided material has to compile with it OFF or the shader rejects
// exactly the faces the hardware was told to keep. SMALLPRIM is off under MSAA (it assumes one sample at
// the pixel center) and under wireframe (a sub-pixel triangle still draws visible edges).
[vk::constant_id(7)] const uint SPEC_TRI_CULL = 0;

static const uint TRI_CULL_BACKFACE  = 1u;
static const uint TRI_CULL_SMALLPRIM = 2u;

// Mesh-stage workgroup width. Narrower than the meshlet on purpose: at 32 threads over a 64/64 meshlet
// each lane owns two vertices and two triangles, and the two chains are independent, so the compiler
// overlaps them instead of stalling one load at a time. Both iteration counts derive from it, so the
// width is a single edit.
#define MESHLET_MESH_GROUP_SIZE 32
#define MESHLET_VERT_ITERS ((MESHLET_MAX_VERTICES  + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)
#define MESHLET_TRI_ITERS  ((MESHLET_MAX_TRIANGLES + MESHLET_MESH_GROUP_SIZE - 1) / MESHLET_MESH_GROUP_SIZE)

// Vertex V's clip position, out of the OWNING LANE'S REGISTERS rather than groupshared.
//
// Correct only because the workgroup is exactly one subgroup: WaveReadLaneAt cannot reach across waves, and
// at MESHLET_MESH_GROUP_SIZE > the hardware subgroup width a triangle could name a vertex another wave
// owns. That is why the mesh workgroup is 32 and each lane carries MESHLET_VERT_ITERS vertices.
//
// Every lane must be active here. The triangle loops clamp their index rather than bounds-branching, which
// is what keeps that true.
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

// True if the triangle cannot contribute a fragment. Operates in CLIP space so a vertex behind the eye
// is handled explicitly rather than divided by a negative w, which reads as a winding flip and would
// cull the front faces of anything straddling the near plane.
bool IsMeshletTriangleCulled(float4 C0, float4 C1, float4 C2, float2 ViewportSize)
{
    // Three vertices outside the SAME plane means no intersection; outside three DIFFERENT planes proves
    // nothing, which is why these are separate tests and not one any(). Only the z >= 0 half is tested:
    // which side is "far" depends on reverse-Z, and depth clipping removes that end for free.
    // Non-short-circuiting on purpose. `&&` and `||` lower to a branch per term, and at five rejects that
    // was ~15 branches before any real work, on operands already sitting in registers. all() over a bool3
    // is arithmetic, and the bitwise OR folds every reject into a single branch.
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

    // Twice the signed area, as the 3x3 determinant of the homogeneous (x, y, w) rows. That determinant
    // equals Area * w0*w1*w2, and every w is positive by the test above, so its SIGN is exactly the NDC
    // cross product's while costing no divides. Roughly half of all triangles are backfacing, and this
    // now rejects them BEFORE the perspective divide, which moved into the small-prim branch below.
    //
    // CmdSetViewport never flips Y and always uses a positive height, so this sign is the one Vulkan
    // computes in framebuffer space; front faces are counter-clockwise.
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
        // The rasterizer samples at pixel CENTERS, so the triangle contributes nothing unless some
        // integer i satisfies Lo <= i + 0.5 <= Hi on BOTH axes. Testing the bounding box keeps this
        // conservative: it can only reject triangles that provably cover no sample.
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
    float4 Color;
};

// Takes the matrices BY VALUE so the caller can hoist the loads. Reaching ModelMatrix through Ref.Inst
// is a dependent chain the compiler cannot overlap on its own, since a PhysicalStorageBuffer pointer may
// alias anything -- Nsight attributed more dependent stall samples to it than to any other instruction
// in VisBuffer phase 1.
FMeshletWorldVertex TransformMeshletVertex(FVertexData V, float4x4 ModelMatrix, float3x3 NormalMatrixWS)
{
    FMeshletWorldVertex W;
    W.WorldPos  = mul(ModelMatrix, float4(V.Position, 1.0)).xyz;
    W.NormalWS  = mul(NormalMatrixWS, V.Normal);
    // Tangents take the forward 3x3 (they lie in the surface); re-orthogonalized per-pixel.
    W.TangentWS = float4(mul((float3x3)ModelMatrix, V.Tangent.xyz), V.Tangent.w);
    W.UV        = V.UV;
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

// Base pass and depth pre-pass MUST derive SV_Position from this exact pair, in this order, so the
// [earlydepthstencil] equal-depth test holds bit-for-bit. Do not fuse them into a view-projection
// product: that changes the rounding and breaks the test.
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
    uint   InstanceIndex               : INSTANCE;
    nointerpolation uint EntityID      : ENTITY_ID;
    nointerpolation uint MaterialIndex : MATERIAL_INDEX;
    nointerpolation uint InstanceFlags : INSTANCE_FLAGS;
    nointerpolation uint MeshletIndex  : MESHLET_INDEX;
    precise float4 Position            : SV_Position;
};

// A mesh shader's output storage is allocated from the DECLARED type at pipeline creation, not from what
// the lanes write, and that allocation caps mesh-workgroup residency: declaring FBaseVertexOutput for a
// depth-only pass reserved 124B per vertex to carry 16B of SV_Position and measured 4.2% occupancy on the
// cascades. Only for passes with no interpolant consumer.
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
