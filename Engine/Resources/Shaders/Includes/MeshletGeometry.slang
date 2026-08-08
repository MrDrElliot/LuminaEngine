#pragma once
#include "SceneGlobals.slang"
#include "Wind.slang"   // material-graph WindAnimation node; every material vertex template needs it

// The meshlet geometry path. Every stage that touches a meshlet -- the amplification stage, the four mesh
// variants, and the deferred reconstruction -- shares what lives here so they can never drift:
//   - resolving a draw-list slot to its instance + meshlet,
//   - loading a vertex (skinned/static),
//   - unpacking a meshlet triangle,
//   - the model->world transform (position / normal / tangent),
//   - the base-pass interpolant layout (FBaseVertexOutput) + how it's filled,
//   - the camera clip transform (base pass and depth pre-pass MUST match for early-Z).
//
// What deliberately stays in the individual shaders (it genuinely differs):
//   - the material $MATERIAL_VERTEX_INPUTS token (text-substituted inline by the material compiler),
//   - the pass-specific final position (camera VP vs shadow VP) and which outputs are written.

// ---------------------------------------------------------------------------------------------------
// Resolve: draw-list slot -> instance + meshlet
// ---------------------------------------------------------------------------------------------------

struct FMeshletRef
{
    FMeshlet        M;
    uint            InstanceID;
    uint            GlobalMeshletIdx;
    // Pre-skin base for the block this ref resolved into. Surface and shadow LODs get separate
    // compacted slices, so the base has to be picked alongside the meshlet range, not at load time.
    uint            SkinnedVertexBase;

    // The instance is REACHED, not stored. Holding an FGPUInstance by value put a
    // PhysicalStorageBuffer pointer (its MeshletHeader) inside a struct that ResolveMeshlet returns,
    // and Slang materializes that pointer in a Function-storage OpVariable without the Aliased or
    // Restrict decoration SPIR-V requires there -- so every module that resolved a meshlet failed
    // spirv-val. Drivers accept it and then apply aliasing assumptions nothing established.
    //
    // Keeping only the index leaves FMeshletRef pointer-free, which is what makes the return legal.
    // Field access through the property auto-derefs, so every Ref.Inst.Field site reads the same as
    // before -- straight out of the retained instance buffer instead of out of a 128-byte copy.
    //
    // Only valid once InstanceID is assigned, and only on a ref that resolved: the failure paths below
    // leave Instances() unchecked. They all zero M, and every caller tests M before touching Inst.
    property FGPUInstance* Inst { get { return Instances() + InstanceID; } }
};

// bShadow selects the instance's shadow meshlet range instead of the surface (base/depth) range.
FMeshletRef ResolveMeshlet(uint MeshletSlot, bool bShadow)
{
    // Same reasoning as the MeshletHeader check below, one level up: the root only carries these two
    // addresses when their buffers exist, and a pass that draws without them page-faults the device
    // rather than producing a wrong pixel. The CPU side refuses the draw too (VisBufferPass); this is
    // the backstop for every other pass that resolves a meshlet.
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

    // A slot this frame never wrote resolves to a plausible-looking instance from an earlier frame, so
    // reject on the tag before anything reads the instance. Post-packed-regions the raster only draws
    // slots the cull filled, so this should never fire -- it is here so that if that ever stops being
    // true the result is missing geometry rather than the wrong geometry.
    if (!Draw.IsFromFrame(Scene().CullData.MeshletDrawTag) || Ref.Inst.MeshletHeader == nullptr)
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    // bShadow no longer selects a meshlet RANGE -- the draw entry names the meshlet outright, because the
    // cull picks the LOD per view and the raster has no way to know which one that was. All it still
    // selects is the pre-skin block, which the skinning pass built per LOD range.
    Ref.SkinnedVertexBase    = bShadow ? Ref.Inst.ShadowSkinnedVertexBase : Ref.Inst.SkinnedVertexBase;

    // The tag says the slot was written THIS FRAME. It says nothing about the index inside it addressing a
    // meshlet this mesh actually has.
    //
    // One bound instead of the old per-LOD pair, and a stricter one: the end of the whole meshlet array,
    // so it no longer matters which LOD the emitting view picked. The "legal for the surface LOD, out of
    // range for the shadow LOD" ambiguity the pair had went away with the LOD-relative index.
    //
    // Unbounded, this indexes past the end of the meshlet array -- whatever bytes follow the allocation.
    // The counts in that garbage then drive SetMeshOutputCounts and the mesh output array writes, which is
    // how a bad index here becomes an out-of-bounds WRITE two steps later.
    if (Draw.MeshletIndex >= Ref.Inst.MeshletTotalCount)
    {
        Ref.SkinnedVertexBase = kNoPreSkinBase;
        Ref.GlobalMeshletIdx  = 0u;
        Ref.M                 = (FMeshlet)0;
        return Ref;
    }

    Ref.GlobalMeshletIdx     = Draw.MeshletIndex;
    Ref.M                    = Ref.Inst.MeshletHeader.Meshlets[Ref.GlobalMeshletIdx];

    // A meshlet the builder produced can never exceed these -- MeshImport caps every buildMeshlets call at
    // exactly MESHLET_MAX_VERTICES/TRIANGLES. So this is not a clamp on legitimate data, it is the point at
    // which garbage stops propagating: past here the counts reach SetMeshOutputCounts, which the spec
    // requires to be within the shader's DECLARED array sizes, and index writes into fixed-size output
    // arrays. Over-declaring leaves the vertices past the workgroup's 64 lanes never written at all --
    // undefined interpolants, which rasterize as a triangle anchored at whatever the unwritten position
    // decodes to. Dropping the whole meshlet instead makes that a visible hole, not a spike and a fault.
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

// ---------------------------------------------------------------------------------------------------
// Task (amplification) payload
//
// Declared here rather than in either stage because BOTH stages have to agree on it byte for byte and
// nothing checks that they do: a payload is not an interface SPIR-V links, it is a block of memory one
// stage writes and the next reads. A drifted layout produces geometry from the wrong meshlet, not a
// compile error. Same reason FMeshlet and FVertex live in Common.slang next to their C++ mirrors.
// ---------------------------------------------------------------------------------------------------

// The push block EVERY stage of a meshlet draw reads -- the amplification stage that culls, and the mesh
// stage that rasters. One struct rather than two because a task and a mesh stage in the same pipeline
// share a single push-constant range: two declarations would be two views of the same bytes, and only one
// of them could be right.
//
// The cull fields are dead weight in the mesh stage and the raster fields are dead weight in the task
// stage, and neither is padding for the other -- a field one stage ignores still has to sit at the offset
// the other expects.
struct FMeshletPassArgs
{
    uint*                   ViewDrawOffsets;   // per-(view, draw) base into MeshletDrawList()
    // per-(view, draw) emit cursor, zeroed each frame. A bare counter: nothing issues a graphics indirect
    // draw for meshlets any more, so the other three FDrawIndirectArguments fields had no reader.
    uint*                   ViewDrawCursors;
    uint*                   ViewDrawCounts;    // per-(view, draw) emit bound
    uint*                   ViewBlockOffsets;  // per-(view, draw) base into the block list
    FMeshletBlock*          BlockList;
    FMeshletDeferred*       DeferList;
    uint*                   DeferOffsets;      // per-(view, draw) base into the defer list
    uint*                   DeferCursors;      // per-(view, draw) append cursor, zeroed each frame

    uint                    ArgBase;           // CullViewIndex*NumDrawsPerView + IndirectDrawOffset
    uint                    CullViewIndex;     // which FCullView this draw culls against
    // Phase 0: whether an occluded meshlet is DEFERRED for a later re-test or simply rejected.
    //
    // Not a property of the view -- a property of WHEN in the frame this draw runs. Deferring only earns
    // anything when the pyramid being tested is stale: the camera's phase-0 raster tests last frame's, so
    // hiding a meshlet there may be wrong and has to be re-checked. A pass that runs after the mid rebuild
    // (translucency, additive) tests THIS frame's own depth, so occluded means genuinely occluded and a
    // defer would only park the meshlet in a region no later draw reads.
    uint                    CanDefer;
    // Phase 1 only: the EARLY view's arg base, whose defer region this late draw consumes.
    uint                    DeferSrcArgBase;
    int                     ShadowDataIndex;   // raster, shadow variants only
    int                     ViewIndex;         // raster, shadow variants only (cascade / face / spot)
    // The viewport this draw rasters into, in pixels -- for a shadow pass the atlas TILE, not the atlas.
    // Feeds the small-primitive reject. Two scalars so the struct needs no interior padding under either
    // block layout; the C++ push mirror matches.
    float                   ViewportW;
    float                   ViewportH;
};
FMeshletPassArgs* MeshletPass() { return GetPassArgs<FMeshletPassArgs>(); }

// One resolved meshlet, 36 bytes.
//
// No ModelMatrix: every consuming mesh shader loads Ref.Inst for EntityID / MaterialIndex / Flags /
// BoneOffset anyway, so the matrix is 64 free bytes off a line already in cache -- and it is
// workgroup-uniform there, so it is a scalar load rather than a vector one. Carrying it made the payload
// 3584 B, and on RDNA the payload spills to a per-draw VRAM ring whose capacity bounds how many task
// workgroups can be in flight; on NVIDIA it competes directly with groupshared.
struct FTaskMeshletEntry
{
    FMeshlet M;                   // amortizes the deepest dependent hop (Header -> Meshlets[idx])
    uint     MeshletSlot;         // draw-list slot; the mesh shader packs it into VisID
    uint     GlobalMeshletIdx;    // index into the mesh's meshlet array, for MakeBaseVertexOutput
};

// 16 + 36*32 = 1168 B.
//
// InstanceID and SkinnedVertexBase are workgroup-scope, not per-lane: one block is one instance, so all
// 32 entries would otherwise carry the same two values.
struct FVisTaskPayload
{
    uint InstanceID;
    uint SkinnedVertexBase;
    uint _Pad0;
    uint _Pad1;
    FTaskMeshletEntry Entries[MESHLET_TASK_GROUP_SIZE];
};

// Rebuilds the ref the task stage already paid for. The mesh shader never calls ResolveMeshlet on this
// path, so every bound check ResolveMeshlet makes has ALREADY been made -- by the task lane that filled
// this entry, which dropped the meshlet rather than publishing it if any of them failed. That is the
// contract: an entry that reached the payload resolved cleanly.
FMeshletRef MakeMeshletRefFromTask(FVisTaskPayload Payload, uint Index)
{
    FMeshletRef Ref;
    Ref.M                 = Payload.Entries[Index].M;
    Ref.GlobalMeshletIdx  = Payload.Entries[Index].GlobalMeshletIdx;
    Ref.InstanceID        = Payload.InstanceID;
    Ref.SkinnedVertexBase = Payload.SkinnedVertexBase;
    return Ref;
}

// ---------------------------------------------------------------------------------------------------
// Vertex + triangle access
// ---------------------------------------------------------------------------------------------------

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
        // Entities the pre-skin budget could not cover blend here instead. Costs the blend per
        // pass rather than once per frame, which is the point: it is the overflow path.
        if (Ref.SkinnedVertexBase == kNoPreSkinBase)
        {
            return DecodePreSkinnedVertex(SkinVertex(Ref.Inst.MeshletHeader, Ref.M, LocalVertIdx, Ref.Inst.BoneOffset));
        }
        return LoadSkinnedVertex(Ref.M, LocalVertIdx, Ref.SkinnedVertexBase);
    }
    return LoadStaticVertex(Ref.Inst.MeshletHeader, Ref.M, LocalVertIdx);
}

// One packed dword = three 8-bit local vertex indices (the meshlet's triangle).
uint3 UnpackMeshletTriangle(FMeshletRef Ref, uint TriIdx)
{
    uint Packed = Ref.Inst.MeshletHeader.Triangles[Ref.M.TriangleOffset + TriIdx];
    return uint3(Packed & 0xFFu, (Packed >> 8u) & 0xFFu, (Packed >> 16u) & 0xFFu);
}

// Mesh-shader index output. Every index a mesh shader emits must address a vertex the workgroup both
// DECLARED (SetMeshOutputCounts) and WROTE, and a local index is eight bits of imported vertex data --
// nothing between the importer and here bounds it against this meshlet's own vertex count. One that
// overruns points the rasterizer at an output slot no lane ever assigned, whose contents are undefined;
// that is the second way an unwritten mesh output reaches the screen, and it looks identical to the
// first (a triangle anchored at whatever the undefined position decodes to).
//
// Collapsing to (0,0,0) makes the primitive degenerate -- zero area, nothing rasterized -- using vertex
// 0, which is written whenever the meshlet has any vertices at all. An index array has no way to say
// "skip this primitive", so a degenerate triangle is the in-bounds way to spell it.
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

// ---------------------------------------------------------------------------------------------------
// Per-triangle culling (MESH path only)
// ---------------------------------------------------------------------------------------------------
//
// The meshlet cull removed whole meshlets; this removes individual triangles inside the ones that
// survived, before they reach the rasterizer. A meshlet is a ~124-triangle patch of ONE surface, so
// roughly half of it faces away from any given view and a distant one projects to a handful of pixels --
// geometry the hardware would otherwise set up, clip, and only then discard at the backface/coverage
// stage, one primitive at a time.
//
// Only the mesh path can do this: it owns SetMeshOutputCounts and can publish a compacted primitive
// array. The VS-emulation path draws a fixed 3-vertices-per-triangle range and has no way to not emit one.
//
// Which tests run is a SPEC CONSTANT rather than a uniform, because both answers are per-PIPELINE and a
// disabled test must cost nothing:
//   - BACKFACE mirrors the pipeline's hardware cull mode. CmdSetCullMode is DYNAMIC state the shader
//     cannot observe, so a two-sided material (ECullMode::None) has to compile with this OFF -- otherwise
//     the shader silently rejects exactly the faces the hardware was told to keep.
//   - SMALLPRIM is off under MSAA (the test assumes a single sample at the pixel centre) and under
//     wireframe (a sub-pixel triangle still draws visible edges).
[vk::constant_id(7)] const uint SPEC_TRI_CULL = 0;

static const uint TRI_CULL_BACKFACE  = 1u;
static const uint TRI_CULL_SMALLPRIM = 2u;

// Survivor bitmask width. MESHLET_MAX_TRIANGLES is 124, so four words cover it -- and the compaction
// below unrolls those four BY NAME, so raising the cap means updating both.
#define MESHLET_TRI_WORDS 4
// Triangles per lane in a MESHLET_MAX_VERTICES-wide workgroup: ceil(124 / 64).
#define MESHLET_TRI_ITERS 2

// True if the triangle cannot contribute a fragment.
//
// Operates in CLIP space (pre-divide) so a vertex behind the eye is handled explicitly rather than
// divided by a negative w -- which mirrors the position, reads as a winding flip, and would cull the
// front faces of anything straddling the near plane.
//
// ViewportSize is the RENDER TARGET viewport in pixels (a shadow pass passes its atlas TILE, not the
// atlas), and is only read when TRI_CULL_SMALLPRIM is set.
bool IsMeshletTriangleCulled(float4 C0, float4 C1, float4 C2, float2 ViewportSize)
{
    // Frustum reject, in clip space so it stays valid for the behind-the-eye case handled below. All
    // three vertices outside the SAME plane means the triangle cannot intersect the volume; three
    // vertices outside three DIFFERENT planes proves nothing, which is why these are separate tests and
    // not one combined any().
    //
    // Vulkan's clip volume is -w <= x,y <= w and 0 <= z <= w. Only the z >= 0 half is tested here: which
    // side is "far" depends on whether the pass is reverse-Z (camera) or standard Z (shadows), and depth
    // clipping already removes that end for free.
    if (C0.x >  C0.w && C1.x >  C1.w && C2.x >  C2.w) return true;
    if (C0.x < -C0.w && C1.x < -C1.w && C2.x < -C2.w) return true;
    if (C0.y >  C0.w && C1.y >  C1.w && C2.y >  C2.w) return true;
    if (C0.y < -C0.w && C1.y < -C1.w && C2.y < -C2.w) return true;
    if (C0.z <  0.0  && C1.z <  0.0  && C2.z <  0.0 ) return true;

    // A vertex on or behind the eye plane makes the perspective divide meaningless, and every test below
    // lives in NDC. The hardware clipper splits such a triangle correctly, so keep it.
    if (C0.w <= 0.0 || C1.w <= 0.0 || C2.w <= 0.0)
    {
        return false;
    }

    const float2 P0 = C0.xy / C0.w;
    const float2 P1 = C1.xy / C1.w;
    const float2 P2 = C2.xy / C2.w;

    // Twice the signed area in NDC. CmdSetViewport never flips Y (the projection matrix bakes Vulkan's
    // +Y-down NDC instead) and always uses a positive height, so the viewport transform preserves
    // orientation and this sign IS the sign Vulkan computes in framebuffer space. Front faces are
    // VK_FRONT_FACE_COUNTER_CLOCKWISE, i.e. positive area.
    const float Area = (P1.x - P0.x) * (P2.y - P0.y) - (P2.x - P0.x) * (P1.y - P0.y);

    if ((SPEC_TRI_CULL & TRI_CULL_BACKFACE) != 0u)
    {
        // Spelled !(Area > 0) rather than (Area <= 0) so a NaN -- which a degenerate vertex deliberately
        // produces -- lands in the reject too. Zero area is rejected either way.
        if (!(Area > 0.0))
        {
            return true;
        }
    }
    else if (!(abs(Area) > 0.0))
    {
        // Two-sided keeps both windings, but a zero-area (or NaN) triangle still rasterizes nothing.
        return true;
    }

    if ((SPEC_TRI_CULL & TRI_CULL_SMALLPRIM) != 0u && all(ViewportSize > 0.0))
    {
        // NDC -> pixels. The rasterizer samples at pixel CENTRES (i + 0.5), so the triangle contributes
        // nothing unless some integer i satisfies Lo <= i + 0.5 <= Hi on BOTH axes. Testing the bounding
        // box rather than the triangle keeps this conservative: it can only reject triangles that
        // provably cover no sample, never ones that graze one.
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

// Turns the workgroup's survivor bitmask into dense output slots, so the primitives a mesh shader emits
// are packed into [0, Total) with no gaps -- SetMeshOutputCounts publishes ONE count, so a "skipped"
// primitive can only be spelled as one that was never emitted.
//
// The four words are unrolled BY NAME rather than looped over an array: indexing a local array with a
// runtime value makes Slang lower it to an indexable temp (scratch memory) in the hottest part of the
// shader, which costs more than the four selects it replaces.
struct FMeshletTriCompaction
{
    uint W0, W1, W2, W3;
    uint P1, P2, P3;    // exclusive prefix of set bits at the start of words 1, 2, 3
    uint Total;

    uint Word(uint WordIdx)
    {
        return (WordIdx == 0u) ? W0 : (WordIdx == 1u) ? W1 : (WordIdx == 2u) ? W2 : W3;
    }

    bool IsKept(uint TriIdx)
    {
        return (Word(TriIdx >> 5u) & (1u << (TriIdx & 31u))) != 0u;
    }

    // Dense index of TriIdx among the survivors. Only meaningful when IsKept(TriIdx).
    uint OutputSlot(uint TriIdx)
    {
        const uint WordIdx = TriIdx >> 5u;
        const uint BitIdx  = TriIdx & 31u;
        const uint Base    = (WordIdx == 0u) ? 0u : (WordIdx == 1u) ? P1 : (WordIdx == 2u) ? P2 : P3;
        // (1 << 0) - 1 == 0 and (1 << 31) - 1 == 0x7FFFFFFF, so both ends of the word are exact.
        return Base + countbits(Word(WordIdx) & ((1u << BitIdx) - 1u));
    }
};

FMeshletTriCompaction MakeTriCompaction(uint W0, uint W1, uint W2, uint W3)
{
    const uint N0 = countbits(W0);
    const uint N1 = countbits(W1);
    const uint N2 = countbits(W2);
    const uint N3 = countbits(W3);

    FMeshletTriCompaction C;
    C.W0 = W0; C.W1 = W1; C.W2 = W2; C.W3 = W3;
    C.P1 = N0;
    C.P2 = N0 + N1;
    C.P3 = N0 + N1 + N2;
    C.Total = C.P3 + N3;
    return C;
}

// A meshlet triangle whose three local indices are not distinct has zero area whatever the transform
// does, so it never needs its vertices resolved. UnpackMeshletTriangleSafe also collapses an
// out-of-range triple to (0,0,0), which this catches as the same case.
bool IsDegenerateTriangleIndices(uint3 Idx)
{
    return Idx.x == Idx.y || Idx.y == Idx.z || Idx.z == Idx.x;
}

// ---------------------------------------------------------------------------------------------------
// Model -> world transform (pre-WPO). Identical for every pass and both draw paths.
// ---------------------------------------------------------------------------------------------------

struct FMeshletWorldVertex
{
    float3 WorldPos;     // pre-WorldPositionOffset
    float3 NormalWS;
    float4 TangentWS;    // xyz: world tangent, w: MikkTSpace handedness
    float2 UV;
    float4 Color;
};

// Overload taking a precomputed world normal matrix: the deferred reconstruction transforms three
// vertices of one triangle, all sharing the instance's ModelMatrix, so the inverse-transpose is computed
// once by the caller instead of three times here.
// Takes the model matrix BY VALUE so the caller can hoist the load. Reaching it as Ref.Inst.ModelMatrix
// is a dependent chain (scene root -> Instances() -> +InstanceID -> 64B matrix) that the compiler cannot
// hoist or overlap on its own: a PhysicalStorageBuffer pointer may alias anything, so each deref has to
// complete before the next issues. Nsight attributed more dependent stall samples to these loads than to
// any other instruction in VisBuffer phase 1.
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
    // One load feeding both uses. This used to read Ref.Inst.ModelMatrix twice -- once here for the
    // normal matrix and again inside the overload above.
    const float4x4 ModelMatrix = Ref.Inst.ModelMatrix;
    return TransformMeshletVertex(V, ModelMatrix, CalculateNormalMatrix(ModelMatrix));
}

// Camera view/clip. Base pass and depth pre-pass MUST derive SV_Position from this exact pair so the
// [earlydepthstencil] equal-depth test holds bit-for-bit. Both go through CameraViewPosition first.
//
// The matrix-taking overloads exist for the same reason as above -- so a mesh shader can issue the two
// camera loads up front rather than at the end of the vertex chain. They are the SAME two muls in the
// same order, so SV_Position stays bit-identical either way; do not fuse them into a view-projection
// product, which would change the rounding and break the equal-depth test.
float4 CameraViewPosition(float3 WorldPos, float4x4 CameraView) { return mul(CameraView, float4(WorldPos, 1.0)); }
float4 CameraClipPosition(float4 ViewPos, float4x4 CameraProj)  { return mul(CameraProj, ViewPos); }

float4 CameraViewPosition(float3 WorldPos) { return CameraViewPosition(WorldPos, GetCameraView()); }
float4 CameraClipPosition(float4 ViewPos)  { return CameraClipPosition(ViewPos, GetCameraProjection()); }

// ---------------------------------------------------------------------------------------------------
// Base-pass interpolants: the single source of truth, produced by the VS emulation AND the mesh shader,
// and consumed by BasePixelPass. Per-instance constants are forwarded flat so the PS never re-reads
// the 128B FGPUInstance per pixel.
// ---------------------------------------------------------------------------------------------------

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

// Depth-only geometry output: position and nothing else. A mesh shader's output storage is allocated from
// the declared type at pipeline creation, NOT from what the lanes actually write -- so a depth-only pass
// that declares FBaseVertexOutput reserves 124B per vertex (7.9KB per workgroup at MESHLET_MAX_VERTICES) to
// carry 16B of SV_Position. That allocation is what caps mesh-workgroup residency, and the cascade pass was
// measured at 2.0 active warps/cycle (4.2% occupancy) because of it. Zeroing the interpolants at runtime,
// as the old degenerate-output path did, does not give the storage back.
//
// Only for passes with no interpolant consumer: the cascades bind no pixel shader at all, and the point/spot
// shadow PS (ShadowMappingPixel.slang) declares no fragment inputs.
struct FDepthOnlyVertexOutput
{
    precise float4 Position : SV_Position;
};

// FinalWorldPos is post-WPO (the shader adds Material.WorldPositionOffset before calling this).
// Matrix-taking overload for callers that already hoisted the camera pair (see the mesh shaders).
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
