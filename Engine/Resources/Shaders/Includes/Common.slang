#pragma once

#include "Shared/SharedConstants.h"

static const float PHI    = 1.618033988749895;
static const float PI     = 3.1415926535897932384626433832795;
static const float TWO_PI = 6.28318530718;
static const float INV_PI = 0.31830988618;


#define SHADOW_SAMPLE_COUNT 8
// Upper bound for the directional cascade's runtime-tunable tap count
// (SDirectionalLightComponent::ShadowSampleCount, forwarded via ShadowParams2.y).
#define SHADOW_MAX_SAMPLE_COUNT 16

#define INDEX_NONE -1

// Bits inside FCullView.Flags, gating which culling stages run for the view.
#define CULL_VIEW_FLAG_FRUSTUM          BIT(0)
#define CULL_VIEW_FLAG_CONE             BIT(1)
// The meshlet SUB-PIXEL reject only. It gates no pyramid read at all -- that is
// CULL_VIEW_FLAG_MESHLET_HIZ -- because a projected-size test is exact and a pyramid read is not.
#define CULL_VIEW_FLAG_OCCLUSION        BIT(2)
#define CULL_VIEW_FLAG_DISTANCE         BIT(3)
#define CULL_VIEW_FLAG_CAST_SHADOW_ONLY BIT(4)
#define CULL_VIEW_FLAG_SUN_ALIGNED      BIT(5)
// Marks a view as TWO-PHASE, and is the only thing that does. A single-phase view (shadow, capture,
// probe) must leave it clear: the meshlet cull emits such a view entirely from its early dispatch and
// skips it in the late one, and setting this would make both dispatches emit it.
#define CULL_VIEW_FLAG_MESHLET_HIZ      BIT(6)
#define CULL_VIEW_FLAG_CASCADE          BIT(7)


#define BIT(X) (1u << (X))

// Mirror of ELightFlags in SceneRenderTypes.h -- keep values in lockstep.
// Type bits stay as bits (not a sequential enum) so feature flags can OR in.
enum ELightFlags
{
    None        = 0,
    Directional = BIT(0),
    Point       = BIT(1),
    Spot        = BIT(2),
    CastShadow  = BIT(3),
    // Light scatters through participating media; sampled by VolumetricLighting.slang.
    Volumetric  = BIT(4),
}

enum EInstanceFlags
{
    None            = 0,
    Billboard       = BIT(0),
    Skinned         = BIT(1),
    CastShadow      = BIT(2),
    ReceiveShadow   = BIT(3),
    TwoSided        = BIT(4),  // Skip backface cone cull.
    NoCull          = BIT(5),
    Translucent     = BIT(6),
    Masked          = BIT(7),

    // Retained cull entries only; meaningless on a compacted output instance.
    Active          = BIT(8),   // 0 = free slot; the cull skips it without reading its payload
    HasGeometry     = BIT(9),   // the mesh's meshlet header is resident
}

// Mirror of Lumina::FMeshletVertex. Position is three 16-bit offsets from the meshlet anchor -- decode
// with DecodeMeshletPosition, never by hand. Tangent is 15-15 octahedral + 1-bit handedness.
struct FMeshletVertex
{
    uint16_t PositionX;
    uint16_t PositionY;
    uint16_t PositionZ;
    int16_t  NormalX;
    int16_t  NormalY;
    int16_t  NormalZ;
    uint     Tangent;
    uint     UV;    // TEXCOORD_0
    uint     UV1;   // TEXCOORD_1; mirrors UV for single-set sources
    uint     Color;
};

struct FMeshletSkinnedVertex
{
    uint16_t PositionX;
    uint16_t PositionY;
    uint16_t PositionZ;
    int16_t  NormalX;
    int16_t  NormalY;
    int16_t  NormalZ;
    uint     Tangent;
    uint     UV;    // TEXCOORD_0
    uint     UV1;   // TEXCOORD_1
    uint     Color;
    uint     JointIndices;
    uint     JointWeights;
};

float SNorm16ToFloat(int16_t V)
{
    return max((float)V * (1.0 / 32767.0), -1.0);
}

float3 UnpackNormalSNorm16(int16_t X, int16_t Y, int16_t Z)
{
    return float3(SNorm16ToFloat(X), SNorm16ToFloat(Y), SNorm16ToFloat(Z));
}

int16_t FloatToSNorm16(float V)
{
    return (int16_t)(clamp(V, -1.0, 1.0) * 32767.0 + (V >= 0.0 ? 0.5 : -0.5));
}

// Mirror of Lumina::FPreSkinnedVertex. Position stays fp32 because skinning breaks meshlet quantization.
struct FPreSkinnedVertex
{
    float    Px;
    float    Py;
    float    Pz;
    uint     Normal;    // PackNormal
    uint     Tangent;   // PackTangent
    uint     UV;        // carried through from the source vertex (skin-invariant)
    uint     UV1;       // likewise
    uint     Color;
};

struct FVertexData
{
    float3 Position;
    float3 Normal;
    float4 Tangent;    // xyz: tangent direction; w: handedness sign for B = cross(N,T)*w
    float2 UV;         // TEXCOORD_0
    float2 UV1;        // TEXCOORD_1
    float4 Color;
};

struct FDrawIndirectArguments
{
    uint VertexCount;
    uint InstanceCount;
    uint VertexOffset;
    uint FirstInstance;
};

struct FDrawIndexedIndirectArguments
{
    uint IndexCount;
    uint InstanceCount;
    uint FirstIndex;
    int  VertexOffset;
    uint FirstInstance;
};

struct FCameraView
{
    float4 CameraPosition;
    float4 CameraUp;
    float4 CameraRight;
    float4 CameraForward;
    float4x4 CameraView;
    float4x4 InverseCameraView;
    float4x4 CameraProjection;
    float4x4 InverseCameraProjection;
    float4x4 PrevViewProjection;
};

struct FMaterialPixelInputs
{
    float3 Diffuse;
    float  Metallic;
    float  Roughness;
    float  Specular;
    float3 Emissive;
    float  AmbientOcclusion;
    float3 Normal;
    float  Opacity;
    // Height-field self-occlusion against the SUN specifically, so it modulates only the sun -- not
    // ambient (AmbientOcclusion's job) and not the punctual lights, which were never traced. 1 = unshadowed.
    float  SelfShadow;

    // Clearcoat shading model only: a clear dielectric layer over the base. Strength 0 disables it.
    // Ignored entirely by every other model, and the GBuffer does not carry them for those.
    float  Clearcoat;
    float  ClearcoatRoughness;

    // Foliage shading model only, and it costs the emissive channels when that model is selected.
    float3 Transmission;
    float  Thickness;
};

// EVERY producer of FMaterialPixelInputs must start from this and then override: the struct has
// several independent authors, so a new field would otherwise read as garbage, not a compile error.
FMaterialPixelInputs DefaultMaterialInputs()
{
    FMaterialPixelInputs M;
    M.Diffuse          = float3(1.0, 1.0, 1.0);
    M.Metallic         = 0.0;
    M.Roughness        = 1.0;
    M.Specular         = 0.5;
    M.Emissive         = float3(0.0, 0.0, 0.0);
    M.AmbientOcclusion = 1.0;
    M.Normal           = float3(0.0, 0.0, 1.0);
    M.Opacity            = 1.0;
    M.SelfShadow         = 1.0;
    M.Clearcoat          = 0.0;
    M.ClearcoatRoughness = 0.03;
    M.Transmission       = float3(0.0, 0.0, 0.0);
    M.Thickness          = 0.0;
    return M;
}

struct FMaterialVertexInputs
{
    float3 WorldPositionOffset;
};

struct FEntityAABB
{
    uint  Entity;
    float MinX;
    float MinY;
    float MaxX;
    float MaxY;
};

// GTAO tuning; matches C++ FGTAOSettings (SceneRenderTypes.h). Slice directions come from
// per-pixel interleaved gradient noise in the shader, no CPU kernel.
struct FGTAOSettings
{
    float  Radius;
    float  Intensity;
    float  Power;
    float  Bias;             // reserved (unused by GTAO)
    uint   AOTextureIndex;    // bindless SRV the base pass samples; ~0u = no GTAO for this view
    uint   _Pad0;
    uint   _Pad1;
    uint   _Pad2;
};

static const uint MatFlag_None        = 0;
static const uint MatFlag_Masked      = BIT(0);
static const uint MatFlag_Translucent = BIT(1);
static const uint MatFlag_Additive    = BIT(2);
// Bits 3-5 are a 3-bit shading model field (EMaterialGPUFlags in MaterialTypes.h), not one flag per
// model. MatFlag_Unlit is the legacy spelling of ShadingModel_Unlit sitting in that field.
static const uint MatFlag_Unlit       = BIT(3);

#define MATERIAL_SHADING_MODEL_SHIFT 3u
#define MATERIAL_SHADING_MODEL_MASK  7u

/** The material's shading model, as an EShadingModel value (GBuffer.slang). */
uint GetMaterialShadingModel(uint MatFlags)
{
    return (MatFlags >> MATERIAL_SHADING_MODEL_SHIFT) & MATERIAL_SHADING_MODEL_MASK;
}

struct FMaterialUniforms
{
    float4  Vectors[MAX_VECTORS];
    float   Scalars[MAX_SCALARS];
    uint    Textures[MAX_TEXTURES];
    uint    Flags;
    float   OpacityClipValue;
    uint    Padding[2];
};

struct FCustomPrimitiveData
{
    uint Packed;

    property float  AsFloat     { get { return asfloat(Packed); } }
    property int    AsInt       { get { return asint(Packed); } }
    property uint   AsUInt      { get { return Packed; } }
    property bool   AsBool      { get { return Packed != 0; } }
    property float4 AsColor     { get { return float4(unpack_u8u32(Packed)) / 255.0; } }
}

// Mirror of Lumina::FSurfaceDescGPU (80 B), interned per distinct resolved surface so a cull view can
// pick its own LOD. The sentinel means the LOD is fixed (skinned, forced, or LODs off scene-wide).
static const uint kNoSurfaceDesc = NO_SURFACE_DESC_INDEX;

struct FSurfaceDesc
{
    uint  LODMeshletOffset[MESHLET_MAX_LODS];
    uint  LODMeshletCount[MESHLET_MAX_LODS];
    float LODScreenThresholdSq[MESHLET_MAX_LODS];
    uint  NumLODs;
    uint  _Pad;

    // Squared form of the CPU's (distance / radius >= threshold) test. Monotonic in LOD, so the first
    // threshold that fails ends the walk.
    uint SelectLOD(float DistSq, float RadiusSq)
    {
        if (RadiusSq <= 0.0 || NumLODs <= 1u)
        {
            return 0u;
        }
        uint Picked  = 0u;
        uint LastLOD = NumLODs - 1u;
        for (uint i = 1u; i <= LastLOD; ++i)
        {
            if (DistSq >= LODScreenThresholdSq[i] * RadiusSq)
            {
                Picked = i;
            }
            else
            {
                break;
            }
        }
        return Picked;
    }
}

// Mirror of Lumina::FMeshlet. Positions are full float3 in the vertex buffer (no quantization). LODIndex
// kept for LOD selection.
struct FMeshlet
{
    uint VertexOffset;
    uint TriangleOffset;
    uint VertexCount;
    uint TriangleCount;
    uint LODIndex;

    // bits 0..23 = signed 24-bit anchor component. PackedAnchorX additionally carries the signed
    // 8-bit shared exponent in bits 24..31; the Y and Z high bytes are reserved.
    uint PackedAnchorX;
    uint PackedAnchorY;
    uint PackedAnchorZ;
};

// Meshlet position decode (DXR2 COMPRESSED1). Twin of the block in MeshData.h -- these two are the
// ONLY places that know the encoding, and a one-ULP disagreement breaks early-Z and barycentrics.

int UnpackMeshletAnchor(uint Packed)
{
    // Sign-extend from 24 bits.
    uint Field = Packed & MESHLET_ANCHOR_MASK;
    return int(Field ^ MESHLET_ANCHOR_SIGN) - int(MESHLET_ANCHOR_SIGN);
}

int UnpackMeshletExponent(uint PackedAnchorX)
{
    // Sign-extend the signed 8-bit exponent out of the high byte.
    uint Byte = (PackedAnchorX >> MESHLET_EXPONENT_SHIFT) & 0xFFu;
    return int(Byte ^ 0x80u) - int(0x80u);
}

// Exponent -> exact power of two, built from the float exponent field rather than exp2 so both
// languages produce identical bits and the decode stays one multiply instead of a transcendental.
float MeshletExponentScale(int Exponent)
{
    return asfloat(uint(clamp(Exponent, -126, 127) + 127) << 23);
}

float3 DecodeMeshletPosition(FMeshlet M, uint16_t PosX, uint16_t PosY, uint16_t PosZ)
{
    float Scale = MeshletExponentScale(UnpackMeshletExponent(M.PackedAnchorX));

    int Ox = int(PosX);
    int Oy = int(PosY);
    int Oz = int(PosZ);

    return float3(
        float(UnpackMeshletAnchor(M.PackedAnchorX) + Ox) * Scale,
        float(UnpackMeshletAnchor(M.PackedAnchorY) + Oy) * Scale,
        float(UnpackMeshletAnchor(M.PackedAnchorZ) + Oz) * Scale);
}

// Mirror of Lumina::FMeshletBounds. Sphere + cone drive culling.
struct FMeshletSphere
{
    float3 Center;
    float  Radius;
};

struct FMeshletCone
{
    float3 Apex;
    float  Cutoff;
    float3 Axis;
};

// Mirror of Lumina::FMeshletHeaderGPU. The distance-field block is loose floats, not a float3: scalar
// layout aligns float3 to 4 bytes while the C++ side may over-align FVector3 to 16.
// Mirror of Lumina::FMeshletBonePalette. One meshlet's slice of FMeshletHeader::BoneIndices.
struct FMeshletBonePalette
{
    uint Offset;
    uint Count;
};

struct FMeshletHeader
{
    FMeshlet*       Meshlets;
    FMeshletSphere* Spheres;
    uint*           Vertices;
    uint*           Triangles;
    FMeshletCone*   Cones;

    // Null unless the mesh is skinned. A skinned vertex's JointIndices address BonePalettes[meshlet].
    FMeshletBonePalette* BonePalettes;
    uint*                BoneIndices;

    uint            DistanceFieldIndex;     // gTextures3D[] slot, or kInvalidDistanceField
    uint            DistanceFieldFlags;     // bit0 = two-sided (unsigned) encoding

    float           DistanceFieldMinX, DistanceFieldMinY, DistanceFieldMinZ;
    float           DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ;
    float           DistanceFieldMaxDistance;

    // Entries in Meshlets/Spheres/Cones. The AUTHORITATIVE bound for every array above, because it ships
    // in the same 96 bytes as the pointers. Everything that indexes them bounds against THIS rather than
    // against a count carried on the instance: the two come from different sources, and a stale instance
    // paired with a live header is exactly how an in-bounds-looking index walks off a buffer.
    //
    // The null header (slab slot 0) has this at 0 and every pointer null, so an instance naming a mesh
    // that has died rejects here before it dereferences anything. That is why none of these pointers is
    // null-checked anywhere.
    uint            MeshletCount;

    property float3 DistanceFieldMin  { get { return float3(DistanceFieldMinX,  DistanceFieldMinY,  DistanceFieldMinZ);  } }
    property float3 DistanceFieldSize { get { return float3(DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ); } }
};

// Mirror of Lumina::FInstanceCullEntry (32 B): the CULL-HOT half only, so two slots share a cache
// line. What a SURVIVOR needs lives elsewhere and is not loaded until this says the instance is visible.
struct FInstanceCullEntry
{
    float4 SphereBounds;      // world space; xyz = center, w = radius
    uint   DrawIDAndFlags;    // same packing as FGPUInstance; carries Active and HasGeometry
    uint   SurfaceDescIndex;
    float  MaxDrawDistance;   // 0 = never distance-culled
    int    ForcedLODIndex;    // -1 = automatic

    bool HasFlag(EInstanceFlags Flag)
    {
        uint Mask = (uint)Flag << 16;
        return (DrawIDAndFlags & Mask) == Mask;
    }

    property uint DrawID { get { return DrawIDAndFlags & 0xFFFFu; } }

    property EInstanceFlags Flags { get { return EInstanceFlags((DrawIDAndFlags >> 16) & 0xFFFFu); } }
}

// Mirror of Lumina::FInstanceStatic (32 B). The retained scene's COLD half: what a SURVIVING instance
// needs copied into the compacted output, and which changes only on a re-bind -- never on a move.
struct FInstanceStatic
{
    // A SLOT in the header slab, never an address -- see FGPUInstance::MeshletHeader below.
    uint            MeshletHeaderSlot;
    FCustomPrimitiveData CustomData;   // one packed uint; same as FGPUInstance's
    uint            MaterialIndex;
    uint            EntityID;
    uint            BoneOffset;
    uint            SkinnedVertexBase;
    uint            ShadowSkinnedVertexBase;
    uint            _Pad0;
}

// Sentinel: this entity was not pre-skinned this frame (it fell outside r.Skinning.MaxVertices), so
// the draw path blends bones inline. The CPU reserves the value, so a wrapped base cannot collide.
static const uint kNoPreSkinBase = 0xFFFFFFFFu;
static const uint kNoBoneSlice   = 0xFFFFFFFFu;

// Mirror of Lumina::FSkinnedFrameData. Per-frame values for one SKINNED instance slot, indexed by RETAINED
// slot: what FInstanceStatic cannot hold because it re-uploads only on a re-bind. The LOD ranges travel with
// the pre-skin bases deliberately -- the slice was assigned against this pick, and a view that re-selected a
// different LOD would pre-skin one LOD's meshlets while the raster read another's vertices.
struct FSkinnedFrameData
{
    // CullData.MeshletDrawTag as of the writing frame. The array is never cleared, so a slot whose tag does
    // not match was not gathered this frame and must not be emitted.
    uint FrameTag;
    uint SurfaceMeshletOffset;
    uint SurfaceMeshletCount;
    uint ShadowMeshletOffset;
    uint ShadowMeshletCount;
    uint MeshletTotalCount;
    uint SkinnedVertexBase;
    uint ShadowSkinnedVertexBase;
    // Slice in the COMPACTED per-frame arena; kNoBoneSlice falls the blend back to identity.
    uint BoneOffset;

    bool IsFromFrame(uint ExpectedTag) { return FrameTag == ExpectedTag && ExpectedTag != 0u; }
};

// Mirror of Lumina::FTransform3x4: affine with the always-(0,0,0,1) last row dropped. The rows are
// MATHEMATICAL rows -- Slang stores float4x4 ColMajor but float4x4(v0..v3) fills ROWS (verified).
struct FTransform3x4
{
    float4 Row0;
    float4 Row1;
    float4 Row2;

    // Restores the dropped (0,0,0,1) row. The result is bit-identical to the float4x4 the CPU packed.
    float4x4 ToMatrix()
    {
        return float4x4(Row0, Row1, Row2, float4(0.0, 0.0, 0.0, 1.0));
    }
};

// Mirror of Lumina::FPackedBoneTransform: 3x3 in halves, translation left at full precision.
struct FBoneTransform
{
    uint  Rot[5];
    float Tx;
    float Ty;
    float Tz;
};

// One entry per surviving meshlet, stamped with the frame that wrote it: capacity bounds are
// fault-safe but not correctness-safe, so the tag turns a stale slot into a reject, not geometry.

#define MESHLET_DRAW_INDEX_MASK  ((1u << MESHLET_DRAW_INDEX_BITS) - 1u)
#define MESHLET_DRAW_TAG_INVALID 0u

struct FMeshletDraw
{
    uint InstanceID;          // Into Instances()
    uint PackedIndexAndTag;   // [19:0] index into the mesh's meshlet array, [31:20] frame tag

    property uint MeshletIndex { get { return PackedIndexAndTag & MESHLET_DRAW_INDEX_MASK; } }
    property uint Tag          { get { return PackedIndexAndTag >> MESHLET_DRAW_INDEX_BITS; } }

    // Written this frame? Compare against Scene().CullData.MeshletDrawTag.
    bool IsFromFrame(uint ExpectedTag) { return Tag == ExpectedTag && ExpectedTag != MESHLET_DRAW_TAG_INVALID; }

    static uint Pack(uint MeshletIndex, uint FrameTag)
    {
        return (FrameTag << MESHLET_DRAW_INDEX_BITS) | (MeshletIndex & MESHLET_DRAW_INDEX_MASK);
    }
};

// Up to MESHLET_CULL_GROUP_SIZE consecutive meshlets of ONE instance with its range for ONE view
// resolved. The length is load-bearing: a partial block's spare lanes would address the NEXT LOD.
struct FMeshletBlock
{
    uint InstanceID;          // slot in the visible-instance buffer
    // MeshletBase[19:0] | Count[25:20]. Base is MESH-GLOBAL -- the view's LOD pick is already folded in,
    // so the cull makes no LOD decision at all.
    uint PackedBaseAndCount;

    static uint Pack(uint MeshletBase, uint Count)
    {
        return (MeshletBase & MESHLET_DRAW_INDEX_MASK) | ((Count & 63u) << MESHLET_DRAW_INDEX_BITS);
    }

    property uint MeshletBase { get { return PackedBaseAndCount & MESHLET_DRAW_INDEX_MASK; } }
    property uint Count       { get { return (PackedBaseAndCount >> MESHLET_DRAW_INDEX_BITS) & 63u; } }
};

// One (view, draw) pair's slice of the cull arenas. Base, capacity and cursor travel together --
// as parallel arrays, pairing a clamped base with an unclamped length overran the allocation.
struct FRenderBucket
{
    uint DrawBase;
    uint DrawCapacity;
    uint DrawCursor;
    uint BlockBase;
    uint BlockCapacity;
    uint BlockCursor;
    // Dense offset of this bucket's blocks in the flat meshlet-cull dispatch. Blocks are dense within a
    // bucket but sparse across the arena, so a workgroup cannot read its bucket from the block itself.
    uint CullWorkBase;
    // What each cull phase appended, RELATIVE to DrawBase (DrawCursor is a count, not an index).
    // Indexed by MESHLET_SLICE_*: both VisBuffer phases share cull view 0, so one pair would be
    // clobbered by the late phase before the single-phase passes draw.
    uint SliceBase[MESHLET_SLICE_COUNT];
    uint SliceCount[MESHLET_SLICE_COUNT];
    // Sub-draws each slice needs, read as the DRAW COUNT by vkCmdDrawMeshTasksIndirectCountEXT at
    // offsetof(SubDrawCount) + slice * 4. Last field on purpose: the count offset is its byte offset.
    uint SubDrawCount[MESHLET_SLICE_COUNT];
};

// Mirror of Lumina::FGPUInstance.
struct FGPUInstance
{
    // 3x4, not 4x4: an instance transform is always affine. Read it through the ModelMatrix property
    // below, which expands it -- that is why every consumer of this struct was untouched by the change.
    FTransform3x4           Transform;
    float4                  SphereBounds;
    // A SLOT in the process-wide header slab, never an address. A header address cached in scene state
    // outlives the mesh that owns it -- the retained instance buffer re-emits it into frames recorded
    // long after the mesh died -- and a slot cannot dangle: the worst it can do is name the null header.
    // Reached through the MeshletHeader property (SceneGlobals.slang), which is why nothing below changed.
    uint                    MeshletHeaderSlot;
    uint                    _HeaderPad0;

    // Explicitly zeroed: Slang's synthesized default initializer skips private members, so (FGPUInstance)0
    // left the packed draw id and flags holding whatever was on the stack.
    private uint            DrawIDAndFlags = 0u;
    // Meshlet range of the DEFAULT surface LOD, used verbatim by any view that cannot re-select and as
    // the reference the per-view pick starts from. The raster resolves a mesh-global index instead.
    uint                    SurfaceMeshletOffset;
    uint                    SurfaceMeshletCount;
    // Shadow views keep this pick rather than re-selecting: the cascades are fit to the PRIMARY camera,
    // so the LOD matching their texel pitch is a function of that camera's distance, not the light's.
    uint                    ShadowMeshletOffset;
    uint                    ShadowMeshletCount;
    FCustomPrimitiveData    CustomData;

    // Full 32-bit index into the global bone-matrix buffer (no longer packed with the
    // material index), so the scene is not capped at 64k total bones.
    uint                    BoneOffset;
    uint                    MaterialIndex;
    uint                    EntityID;
    // Bases into PreSkinnedVertices() for the surface-LOD and shadow-LOD blocks. Equal when both resolved
    // to the same block, 0 for static, kNoPreSkinBase when the entity lost the pre-skin budget.
    uint                    SkinnedVertexBase;
    uint                    ShadowSkinnedVertexBase;
    // Into SurfaceDescs, so a cull view can re-select this instance's LOD from its OWN origin.
    // kNoSurfaceDesc means "not selectable": use SurfaceMeshletOffset/Count as they stand.
    uint                    SurfaceDescIndex;
    // Entries in this mesh's meshlet array (end of the last LOD). The bound on a mesh-global meshlet index,
    // and the only bound the raster resolve needs now that the index is not LOD-relative.
    uint                    MeshletTotalCount;
    // Index into InstanceVisibility, the two-phase occlusion flag. The compacted slot is re-derived every
    // frame, so the retained slot is the only key that means the same instance next frame.
    uint                    RetainedSlot;

    // Expands the packed 3x4. Named for what it returns so every consumer predates the packing and did
    // not have to change; the struct is loaded once and this is a few moves off registers.
    property float4x4 ModelMatrix { get { return Transform.ToMatrix(); } }

    // DrawID low 16, flags high 16 -- matches Lumina::PackDrawIDAndFlags. The flag field is 16 bits wide
    // so FInstanceCullEntry has somewhere to carry Active and HasGeometry without a 33rd byte.
    bool HasFlag(EInstanceFlags Flag)
    {
        uint Mask = (uint)Flag << 16;
        return (DrawIDAndFlags & Mask) == Mask;
    }

    property uint DrawID
    {
        get { return DrawIDAndFlags & 0xFFFFu; }
        set { DrawIDAndFlags = (DrawIDAndFlags & 0xFFFF0000u) | (newValue & 0xFFFFu); }
    }

    property EInstanceFlags Flags
    {
        get { return EInstanceFlags((DrawIDAndFlags >> 16) & 0xFFFFu); }
        set { DrawIDAndFlags = (DrawIDAndFlags & 0x0000FFFFu) | ((newValue & 0xFFFFu) << 16); }
    }

    property float3 BoundsCenter { get { return SphereBounds.xyz; } }
    property float  BoundsRadius { get { return SphereBounds.w; } }
};

struct FLightShadow
{
    float2 AtlasUVOffset;
    float2 AtlasUVScale;
    int    ShadowMapIndex;
    int    LightIndex;
    int    ShadowDataIndex;
    int    _Padding;
};

// 32 B, must match FBillboardInstance in SceneRenderTypes.h (scalar layout needs the explicit pad).
struct FBillboardInstance
{
    float3          Position;
    float           Size;

    private uint    ColorPack;
    uint            TextureIndex;
    uint            EntityID;
    uint            _Pad0;

    property float3 ColorTint { get { return UnpackColor(ColorPack).rgb; }}
};

// World-space UI widget quad. Matches FWidgetInstance on the C++ side (96B, dense).
struct FWidgetInstance
{
    float4x4        Transform;
    float2          WorldSize;
    uint            TextureIndex;
    uint            Flags;          // bit0 = billboard
    private uint    ColorPack;
    uint            EntityID;
    private uint    Pad0;
    private uint    Pad1;

    property float4 Tint { get { return UnpackColor(ColorPack); }}
};

// One local reflection probe. 160 B, must match FGPUReflectionProbe. Extraction bakes the half-extents
// into WorldToProbe, so the influence volume is the unit box or unit sphere in probe space.
struct FGPUReflectionProbe
{
    float4x4    WorldToProbe;
    float4x4    ProbeToWorld;
    float4      CapturePosition;   // xyz = world capture origin, w = 1 once this probe has been baked
    float4      Params;            // x=brightness, y=shape, z=array slice, w=blend fraction

    property float  Brightness   { get { return Params.x; }}
    property bool   IsSphere     { get { return Params.y > 0.5; }}
    property uint   Slice        { get { return (uint)Params.z; }}
    property float  BlendFraction{ get { return Params.w; }}
    // A probe waiting in the bake queue owns a slice but has nothing in it. Skip rather than sample: it
    // would consume influence and return black, darkening instead of letting the sky cover the pixel.
    property bool   IsBaked      { get { return CapturePosition.w > 0.5; }}
};

// One spline control point, world space. 64 B, must match FGPUSplinePoint.
struct FGPUSplinePoint
{
    float3 Location;      float Roll;   // Roll in degrees
    float3 ArriveTangent; float _Pad0;
    float3 LeaveTangent;  float _Pad1;
    float3 Scale;         float _Pad2;
};

// One entry of a spline's arc-length table. 64 B, must match FGPUSplineSample.
// Entries are uniform in DISTANCE (not in curve key), which is what lets a lookup be a divide.
struct FGPUSplineSample
{
    float3 Position; float DistanceAlong;
    float3 Tangent;  float Key;    // curve key (0..NumSegments) this sample landed on
    float3 Up;       float Roll;   // degrees
    float3 Scale;    float _Pad;
};

static const uint SPLINE_FLAG_CLOSED_LOOP = 1u;

// Header for one uploaded spline; points and samples are slices of two shared arrays. 160 B,
// must match FGPUSpline.
struct FGPUSpline
{
    float4x4 LocalToWorld;
    float4x4 WorldToLocal;
    uint     PointOffset;
    uint     PointCount;
    uint     SampleOffset;
    uint     SampleCount;
    float    TotalLength;
    uint     Flags;
    uint     EntityID;
    uint     _Pad;

    property bool IsClosedLoop { get { return (Flags & SPLINE_FLAG_CLOSED_LOOP) != 0u; }}
    // Segment count in key space -- the upper bound of FGPUSplineSample::Key. Must match
    // SSplineComponent::GetNumSegments: fewer than two points is zero segments, closed or not.
    property uint NumSegments  { get { return PointCount < 2u ? 0u : (IsClosedLoop ? PointCount : PointCount - 1u); }}
};

static const float kProbeShapeBox    = 0.0;
static const float kProbeShapeSphere = 1.0;

static const uint WIDGET_FLAG_BILLBOARD = 1u;

// Hot per-light data, sized to a single 64B cache line for one L2 miss per light in the
// lighting loop. ShadowDataIndex points into the cold FLightShadowData[] table, or INDEX_NONE.
struct FLight
{
    float3 Position;
    uint   Color;

    float3 Direction;   // to-light: FROM surface TOWARD the light (sun & spot)
    float  Radius;

    float  Intensity;
    float  Falloff;
    float2 Angles;

    uint   Flags;
    int    ShadowDataIndex;

    // Per-light scattering strength; only meaningful when Flags has ELightFlags::Volumetric.
    float  VolumetricIntensity;
    float  VolumetricScatteringRadius;   // soft-core source radius (fraction of Radius) for spot/point fog
};

// Cold shadow data, fetched only after a light is known to cast a shadow, so it stays out
// of the hot loop's cache footprint. One entry per shadow-casting light, capped at MAX_SHADOWS.
struct FLightShadowData
{
    float4x4     ViewProjection[6];
    FLightShadow Shadow[6];
};

struct FLightData
{
    uint   NumLights;
    uint   bHasIBL;       // 1 = environment IBL cubes valid; 0 = skylight-only flat ambient fallback
    uint   Padding[2];
    float3 SunDirection;   // to-light: FROM surface TOWARD the sun (== Lights[0].Direction)
    uint   bHasSun;
    float4 CascadeSplits;
    float4 CascadeRadii;
    // Per-cascade atlas resolution (pixels per side); progressive packing gives each a different texel size.
    float4 CascadeResolutions;
    // Directional shadow tuning (mirror FSceneLightData::ShadowParams).
    // x = normal-bias scale, y = constant depth bias, z = PCSS softness, w = cascade blend fraction.
    float4 ShadowParams;
    // x = far-cascade distance-fade fraction, y = directional PCF tap count,
    // z = contact-shadow world length (0 = off), w = contact-shadow tap count.
    float4 ShadowParams2;
    float4 AmbientLight;
    FLight Lights[MAX_LIGHTS];
    FLightShadowData Shadows[MAX_SHADOWS];
};

struct FCluster
{
    float4 MinPoint;
    float4 MaxPoint;
    uint   LightIndices[LIGHTS_PER_CLUSTER];
    uint   Count;
};

struct FFrustum
{
    float4 Planes[6];
};

// Per-view cull descriptor, one per render view. Each owns a slice of MeshletDrawList / IndirectArgs
// via IndirectArgsOffset; draw-list regions are packed per (view, draw) on the GPU.
struct FCullView
{
    float4 FrustumPlanes[6];   // 96B
    float4 ViewOriginAndFlags; // 16B: xyz=origin (camera / light pos), w=asuint(flags)
    // Formerly reserved padding (the draw list stopped being sliced per view). Reused by the cascade cull
    // so the struct keeps the 128-byte stride it shares with FCullView in SceneRenderTypes.h.
    uint   CascadeIndex;       // Only meaningful when Flags has CULL_VIEW_FLAG_CASCADE
    float  MinBoundsDiameter;  // Reject bounds thinner than this (world units); 0 = off
    uint   IndirectArgsOffset; // Into uIndirectArgs (v * NumDraws)
    uint   NumDraws;           // Number of indirect slots owned by this view

    property float3 ViewOrigin { get { return ViewOriginAndFlags.xyz; } }
    property uint   Flags      { get { return asuint(ViewOriginAndFlags.w); } }

    bool HasFlag(uint Flag) { return (Flags & Flag) == Flag; }
};

struct FCullData
{
// The camera the CULL evaluates against; separate from the render camera so freezing the cull cannot
// move the picture. Cull code must read these and never CameraView.
    float4   CullCameraPosition;
    float4x4 CullCameraView;
    float4x4 CullCameraProjection;

    FFrustum Frustum;
    FFrustum ShadowFrustum;
    // Tight per-cascade CASTER volume (camera sub-frustum slice swept toward the sun). Not the cascade's
    // ortho box -- those nest, so culling on them alone put every caster in every cascade.
    FFrustum CascadeFrustum[NUM_CASCADES];
    // Cascade transforms that produced the pyramid's CURRENT contents (last frame's).
    float4x4 CascadeHZBViewProjection[NUM_CASCADES];
    float4   CascadeHZBTile[NUM_CASCADES];      // xy = pyramid-UV offset of the tile, zw = UV scale
    float4   CascadeHZBNdcScale[NUM_CASCADES];  // xyz = NDC units per world unit
    // THIS frame's transforms, for the phase-2 re-test against the mid-frame pyramid. Distinct from the
    // pair above whenever the camera moved, and projecting one set with the other mis-tests everything.
    float4x4 CascadeHZBViewProjectionMid[NUM_CASCADES];
    float4   CascadeHZBNdcScaleMid[NUM_CASCADES];
    uint     bFrustumCull;
    uint     bOcclusionCull;
    uint     InstanceNum;
    uint     bHasDirectional;
    float    PyramidWidth;
    float    PyramidHeight;
    float    ShadowMaxDistance;
    uint     bShadowOcclusionCull;
    // Tag the meshlet draw list is stamped with this frame; see FMeshletDraw. Never 0.
    uint     MeshletDrawTag;
    uint     DebugMode;
    uint     MeshletDrawListCapacity;   // entries in MeshletDrawList(); an index bound, not a frame total
    uint     DepthPyramidIndex;
    // Cascade Hi-Z: MAX-reduced, because the cascade atlas is standard-Z (the camera pyramid is reverse-Z
    // and MIN-reduced). Invalid until the first cascade raster has been pyramided.
    uint     CascadePyramidIndex;
    uint     bCascadeHZBValid;
    uint     bCascadeHZBMidValid;   // same, for the mid-frame pyramid phase 2 reads
    float    CascadePyramidWidth;
    float    CascadePyramidHeight;
    uint     CascadePyramidMipCount;
    uint     BoneNum;                   // entries in Bones(); an index bound, not a frame total
    // Shadow LOD inputs live here rather than in one pass's push constants because BOTH the instance cull
    // and the meshlet cull make the per-cascade pick, and a disagreement drops meshlets.
    int      ShadowLODBias;
    float    ShadowCoarseLODDistSq;     // past this, shadow LOD caps at MESHLET_MAX_COARSE_SHADOW_LOD; 0 = never
    uint     _CullPad2;
    uint     _CullPad3;
    uint     _CullPad4;
};

// Debug visualization modes. Must match Lumina::ERenderSceneDebugFlags.
#define DEBUG_MODE_NONE                 0u
#define DEBUG_MODE_UNLIT                1u
#define DEBUG_MODE_MESHLETS             2u
#define DEBUG_MODE_WORLD_NORMAL         3u
#define DEBUG_MODE_SHADING_NORMAL       4u
#define DEBUG_MODE_BASE_COLOR           5u
#define DEBUG_MODE_ROUGHNESS            6u
#define DEBUG_MODE_METALLIC             7u
#define DEBUG_MODE_AMBIENT_OCCLUSION    8u
#define DEBUG_MODE_EMISSIVE             9u
#define DEBUG_MODE_UV                   10u
#define DEBUG_MODE_LIGHT_COMPLEXITY     11u
#define DEBUG_MODE_CLUSTER_GRID         12u
#define DEBUG_MODE_SHADOW_CASCADES      13u
#define DEBUG_MODE_SHADOW_PENUMBRA      14u
#define DEBUG_MODE_GTAO                 15u
#define DEBUG_MODE_MATERIAL_ID          16u
#define DEBUG_MODE_TRIANGLE_ID          17u
// OIT target inspectors for OITResolve.slang. LAYER_COUNT also makes the translucent BasePixelPass
// write 1.0 per fragment into accum.a, turning that channel into a raw fragment count.
#define DEBUG_MODE_OIT_ACCUM_COLOR      18u
#define DEBUG_MODE_OIT_MOMENTS          19u
#define DEBUG_MODE_OIT_TRANSMITTANCE    20u
#define DEBUG_MODE_OIT_LAYER_COUNT      21u
// INFLUENCE answers whether a probe reaches this pixel and which one; RADIANCE answers what it
// captured. Both failure modes look identical in a lit view -- the surface just reflects sky.
#define DEBUG_MODE_PROBE_INFLUENCE      22u
#define DEBUG_MODE_PROBE_RADIANCE       23u
// The rest of the GBuffer. These four channels are subject to the per-model BORROW in GBuffer.slang,
// so each view marks the pixels its channel was not stored for rather than showing the fallback --
// see kDebugNotStored in SurfaceShading.slang.
#define DEBUG_MODE_SPECULAR             24u
#define DEBUG_MODE_SHADING_MODEL        25u
#define DEBUG_MODE_CLEARCOAT            26u
#define DEBUG_MODE_CLEARCOAT_ROUGHNESS  27u
#define DEBUG_MODE_SELF_SHADOW          28u
// The one mode that COMPOSITES instead of replacing: the lit shading stays and the wire is drawn on
// top of it. Deferred lane only -- the wire needs the VisBuffer's triangle barycentrics.
#define DEBUG_MODE_WIREFRAME_OVERLAY    29u
#define DEBUG_MODE_VELOCITY             30u

// Global scaling applied on top of every material's authored POM settings, so a weaker GPU can dial the
// whole feature back without recompiling materials. Matches FParallaxSettings in SceneRenderTypes.h.
struct FParallaxSettings
{
    float SampleScale;         // scales the material's Min/Max sample counts; <= 0 disables POM outright
    float LODBias;             // added to the material's LOD threshold; negative fades POM out nearer
    float ShadowSampleScale;   // scales self-shadow samples; 0 disables self-shadowing globally
    float _Pad0;
};

struct FSceneGlobalData
{
    FCameraView       CameraView;
    uint4             ScreenSize;
    uint4             GridSize;
    float             Time;
    float             DeltaTime;
    float             NearPlane;
    float             FarPlane;
    FGTAOSettings     GTAOSettings;
    FCullData         CullData;
    FParallaxSettings ParallaxSettings;

    // Bindless SRV of this view's screen-space sun-shadow mask. ~0u means the pass did not run for this
    // view, so its opaque pipelines are specialized back to the inline cascade PCSS.
    uint              ShadowMaskIndex;

    // MBOIT moment targets for this view (MomentOIT.slang), read by the translucent shading pass.
    // ~0u = not published; see the sentinel guard in BasePixelPass.slang.
    uint              MomentZerothIndex;
    uint              MomentsIndex;
    uint              _ShadowPad2;
};

// Octahedral 15-15 tangent direction + 1-bit handedness sign, matching Lumina::PackTangent. Returns
// (tangent.xyz, sign); the bitangent is reconstructed as cross(N, tangent) * sign.
float4 UnpackTangent(uint Packed)
{
    int sx = bitfieldExtract(int(Packed),  0, 15);
    int sy = bitfieldExtract(int(Packed), 15, 15);
    float2 e = float2(sx, sy) * (1.0 / 16383.0);

    float3 t = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float ht = max(-t.z, 0.0);
    t.xy -= float2(t.x >= 0.0 ? ht : -ht, t.y >= 0.0 ? ht : -ht);

    float Sign = ((Packed >> 30u) & 1u) != 0u ? 1.0 : -1.0;
    return float4(normalize(t), Sign);
}

// Octahedral 16-16 unit normal, matching Lumina::UnpackNormal.
float3 UnpackNormal(uint Packed)
{
    int sx = bitfieldExtract(int(Packed),  0, 16);
    int sy = bitfieldExtract(int(Packed), 16, 16);
    float2 e = float2(sx, sy) * (1.0 / 32767.0);

    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float hn = max(-n.z, 0.0);
    n.xy -= float2(n.x >= 0.0 ? hn : -hn, n.y >= 0.0 ? hn : -hn);
    return normalize(n);
}

// Inverse of UnpackNormal, matching Lumina::PackNormal.
uint PackNormal(float3 n)
{
    float3 d = normalize(n);
    float2 e = d.xy * (1.0 / (abs(d.x) + abs(d.y) + abs(d.z)));
    if (d.z < 0.0)
    {
        e = (1.0 - abs(float2(e.y, e.x))) * float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    }
    int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | ((uint(q.y) & 0xFFFFu) << 16);
}

// Inverse of UnpackTangent, 15-15 signed octahedral plus a handedness sign at bit 30.
uint PackTangent(float4 t)
{
    float3 d = normalize(t.xyz);
    float2 e = d.xy * (1.0 / (abs(d.x) + abs(d.y) + abs(d.z)));
    if (d.z < 0.0)
    {
        e = (1.0 - abs(float2(e.y, e.x))) * float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    }
    int2 q = int2(round(clamp(e, -1.0, 1.0) * 16383.0));
    uint Packed = (uint(q.x) & 0x7FFFu) | ((uint(q.y) & 0x7FFFu) << 15);
    if (t.w >= 0.0)
    {
        Packed |= (1u << 30);
    }
    return Packed;
}

// Octahedral unit-normal <-> [0,1]^2 for unorm targets. Distinct from PackNormal/UnpackNormal: these
// keep the two oct coords as floats so they survive alpha blending.
float2 OctEncode01(float3 N)
{
    N = normalize(N);
    float2 e = N.xy * (1.0 / (abs(N.x) + abs(N.y) + abs(N.z)));
    if (N.z < 0.0)
    {
        e = (1.0 - abs(float2(e.y, e.x))) * float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    }
    return e * 0.5 + 0.5;
}

float3 OctDecode01(float2 Enc)
{
    float2 e = Enc * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.xy -= float2(n.x >= 0.0 ? t : -t, n.y >= 0.0 ? t : -t);
    return normalize(n);
}


float2 UnpackUV(uint Packed)
{
    uint Lo = Packed & 0xFFFFu;
    uint Hi = Packed >> 16u;
    return float2(f16tof32(Lo), f16tof32(Hi));
}

/** Inverse-transpose of the model 3x3 for normals under non-uniform scale, in cofactor form.
 *  The indexing is ROW-wise and load-bearing: reading these as columns yields inverse(A), which is wrong. */
float3x3 CalculateNormalMatrix(float4x4 ModelMatrix)
{
    // Rows of A, despite what a column-major habit suggests.
    float3 A0 = ModelMatrix[0].xyz;
    float3 A1 = ModelMatrix[1].xyz;
    float3 A2 = ModelMatrix[2].xyz;

    float3 Cof0 = cross(A1, A2);
    float3 Cof1 = cross(A2, A0);
    float3 Cof2 = cross(A0, A1);

    float InvDet = 1.0 / dot(A0, Cof0);   // A0 . (A1 x A2) = det(A^T) = det(A)

    return float3x3(Cof0 * InvDet, Cof1 * InvDet, Cof2 * InvDet);
}

float2 VogelDiskSample(int SampleIndex, int SamplesCount, float Angle)
{
    float GoldenAngle = 2.0 * PI * (1.0 - 1.0 / PHI);
    float r           = sqrt((float(SampleIndex) + 0.5) / float(SamplesCount));
    float theta       = float(SampleIndex) * GoldenAngle + Angle;
    float s, c;
    sincos(theta, s, c);
    return float2(r * c, r * s);
}

// Triangular-PDF dither (Gjol/Playdead). Two decorrelated uniform hashes per channel subtract to a
// triangular distribution: flat noise floor, +/-1 LSB at 8-bit.
float3 ScreenSpaceDither(float2 ScreenPos, float Time)
{
    float3 S0 = float3(dot(float2(171.0, 231.0), ScreenPos.xy + Time));
    float3 S1 = float3(dot(float2(113.0, 271.0), ScreenPos.xy + Time + 13.7));
    float3 R0 = frac(S0 / float3(103.0, 71.0, 97.0));
    float3 R1 = frac(S1 / float3(89.0, 113.0, 127.0));
    return (R0 + R1 - 1.0) / 255.0;
}

uint4 UnpackUIntToUVec4(uint Packed)
{
    return uint4(
        (Packed      ) & 0xFFu,
        (Packed >>  8) & 0xFFu,
        (Packed >> 16) & 0xFFu,
        (Packed >> 24) & 0xFFu
    );
}

float4 UnpackColor(uint ColorMask)
{
    return float4(
        float((ColorMask >> COL_R_SHIFT) & 0xFFu) / 255.0,
        float((ColorMask >> COL_G_SHIFT) & 0xFFu) / 255.0,
        float((ColorMask >> COL_B_SHIFT) & 0xFFu) / 255.0,
        float((ColorMask >> COL_A_SHIFT) & 0xFFu) / 255.0
    );
}

float4 GetLightColor(FLight Light)
{
    return UnpackColor(Light.Color);
}

bool IsApproximatelyEqual(float a, float b, float EPSILON)
{
    return abs(a - b) <= (abs(a) < abs(b) ? abs(b) : abs(a)) * EPSILON;
}

float max3(float3 V)
{
    return max(max(V.x, V.y), V.z);
}

bool HasFlag(uint Value, uint Flag)
{
    return (Value & Flag) != 0u;
}

bool HasFlag(uint Value, ELightFlags Flag)
{
    return (Value & (uint)Flag) != 0u;
}

bool HasAllFlags(uint Value, uint Flag)
{
    return (Value & Flag) == Flag;
}

uint SetFlag(uint Value, uint Flag)
{
    return Value | Flag;
}

uint ClearFlag(uint Value, uint Flag)
{
    return Value & (~Flag);
}

uint ToggleFlag(uint Value, uint Flag)
{
    return Value ^ Flag;
}

// Rec. 709 luma. The single luminance definition for the engine -- do not re-derive the dot inline.
float Luminance(float3 C)
{
    return dot(C, float3(0.2126, 0.7152, 0.0722));
}