#pragma once

#include "Shared/SharedConstants.h"
import RHI;

static const float PHI    = 1.618033988749895;
static const float PI     = 3.1415926535897932384626433832795;
static const float TWO_PI = 6.28318530718;
static const float INV_PI = 0.31830988618;


// Upper bound for the sun cascade's tap count, which the renderer's Shadow Quality forwards through ShadowParams2.y.
#define SHADOW_MAX_SAMPLE_COUNT 32

#define INDEX_NONE -1

// Bits inside FCullView.Flags, gating which culling stages run for the view.
#define CULL_VIEW_FLAG_FRUSTUM          BIT(0)
#define CULL_VIEW_FLAG_CONE             BIT(1)
// Gates only the meshlet sub-pixel reject, never a pyramid read (that is CULL_VIEW_FLAG_MESHLET_HIZ).
#define CULL_VIEW_FLAG_OCCLUSION        BIT(2)
#define CULL_VIEW_FLAG_DISTANCE         BIT(3)
#define CULL_VIEW_FLAG_CAST_SHADOW_ONLY BIT(4)
#define CULL_VIEW_FLAG_SUN_ALIGNED      BIT(5)
// The only two-phase marker, and single-phase views must leave it clear or both meshlet dispatches emit them.
#define CULL_VIEW_FLAG_MESHLET_HIZ      BIT(6)
#define CULL_VIEW_FLAG_CASCADE          BIT(7)
#define CULL_VIEW_FLAG_FAR_CASCADE      BIT(8)
// LOD error projects to the same size at any distance, as it does for a cascade.
#define CULL_VIEW_FLAG_ORTHOGRAPHIC_LOD BIT(9)


#define BIT(X) (1u << (X))

// Mirror of ELightFlags in SceneRenderTypes.h, keep values in lockstep.
enum ELightFlags
{
    None        = 0,
    Directional = BIT(0),
    Point       = BIT(1),
    Spot        = BIT(2),
    CastShadow  = BIT(3),
    Volumetric  = BIT(4),
    // Local lights only.
    ContactShadow = BIT(5),
    // Its atlas tile sits at LIGHT_FUNCTION_SLOT_SHIFT.
    LightFunction = BIT(6),
    // A one-sided rectangle. Angles holds its half width and height, and VolumetricScatteringRadius its packed right axis.
    Area        = BIT(7),
    // Bits 8-15 and 24-31 are not free, see LIGHT_FUNCTION_SLOT_SHIFT and LIGHT_MIN_ROUGHNESS_SHIFT.
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
    Active          = BIT(8),   // a clear bit marks a free slot the cull skips without reading its payload
    HasGeometry     = BIT(9),

    ShadowOnly      = BIT(10),
    NoDecals        = BIT(11),
    NoContactShadows = BIT(12),
    CastFarShadow   = BIT(13),  // also drawn into the sun's far cascade, which every other caster skips
    HasDistanceField = BIT(14),
}

// Mirror of Lumina::FMeshVertexAttributes, a static vertex minus its position.
struct FMeshVertexAttributes
{
    uint Normal;    // PackNormal
    uint Tangent;   // PackTangent
    uint UV;        // TEXCOORD_0
};

// Mirror of Lumina::FMeshletSkinnedVertex.
struct FMeshletSkinnedVertex
{
    uint2 Position;   // on the mesh's PositionGrid
    uint  Normal;     // PackNormal
    uint  Tangent;    // PackTangent
    uint  UV;         // TEXCOORD_0
    uint  UV1;        // TEXCOORD_1
    uint  Color;
    uint  JointIndices;
    uint  JointWeights;
};

// Mirror of Lumina::FPreSkinnedVertex. Position stays fp32 because skinning breaks meshlet quantization.
struct FPreSkinnedVertex
{
    float    Px;
    float    Py;
    float    Pz;
    uint     Normal;    // PackNormal
    uint     Tangent;   // PackTangent
    uint     UV;
    uint     UV1;
    uint     Color;
};

// Last frame's skinned position beside each pre-skinned vertex, so motion vectors need no second skin per pixel.
struct FPrevSkinnedPosition
{
    float X;
    float Y;
    float Z;
};

struct FVertexData
{
    float3 Position;
    float3 Normal;
    float4 Tangent;    // w is the handedness sign, B = cross(N,T)*w
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
    // Height-field self-occlusion of the sun only, 1 is unshadowed.
    float  SelfShadow;

    // Clearcoat shading model only, strength 0 disables it.
    float  Clearcoat;
    float  ClearcoatRoughness;

    // Foliage shading model only, and it costs the emissive channels when that model is selected.
    float3 Transmission;
    float  Thickness;
};

// Every producer of FMaterialPixelInputs must start from this so a new field never reads as garbage.
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

struct FGTAOSettings
{
    FTexture2D AOTexture;       // ~0u means no GTAO for this view
    FTexture2D SunShadowMask;   // Sun occlusion traced past the cascades, invalid when not traced.
    uint       _Pad1;
    uint       _Pad2;
};

static const uint MatFlag_None        = 0;
static const uint MatFlag_Masked      = BIT(0);
static const uint MatFlag_Translucent = BIT(1);
static const uint MatFlag_Additive    = BIT(2);
// Bits 3-5 are the 3-bit shading model field (EMaterialGPUFlags), and MatFlag_Unlit is its legacy Unlit value.
static const uint MatFlag_Unlit       = BIT(3);
// Inverted, so a material saved before the flag existed still reads as receiving decals.
static const uint MatFlag_NoDecals    = BIT(6);

#define MATERIAL_SHADING_MODEL_SHIFT 3u
#define MATERIAL_SHADING_MODEL_MASK  7u

uint GetMaterialShadingModel(uint MatFlags)
{
    return (MatFlags >> MATERIAL_SHADING_MODEL_SHIFT) & MATERIAL_SHADING_MODEL_MASK;
}

// Mirror of Lumina::FMaterialHeaderGPU, one per material slot, naming its packed block in the data pool.
struct FMaterialHeader
{
    uint    Flags;
    float   OpacityClipValue;
    uint    DataWord;
    uint    Layout;

    property uint NumVectors  { get { return Layout & MATERIAL_COUNT_MASK; } }
    property uint NumScalars  { get { return (Layout >> MATERIAL_SCALAR_COUNT_SHIFT) & MATERIAL_COUNT_MASK; } }
    property uint NumTextures { get { return (Layout >> MATERIAL_TEXTURE_COUNT_SHIFT) & MATERIAL_COUNT_MASK; } }
    property uint ScalarWord  { get { return DataWord + NumVectors * 4u; } }
    property uint TextureWord { get { return ScalarWord + NumScalars; } }

    // Slot 0 is the reserved zero collection, so an unbound entry reads zeros without a sentinel test.
    uint CollectionIndex(uint Slot)
    {
        return (Layout >> (MATERIAL_COLLECTION_SHIFT + Slot * MATERIAL_COLLECTION_BITS)) & MATERIAL_COLLECTION_MASK;
    }
};

// Mirror of Lumina::FMaterialTableGPU. A block stops at each kind's last nonzero entry, so reads past it are zero.
struct FMaterialTable
{
    GPUSpan<FMaterialHeader> Headers;
    GPUSpan<uint>            Data;

    FMaterialHeader GetHeader(uint MatIndex) { return Headers.GetAligned(MatIndex); }

    float Scalar(uint MatIndex, uint Index)
    {
        const FMaterialHeader Header = GetHeader(MatIndex);
        return Index < Header.NumScalars ? asfloat(Data.At(Header.ScalarWord + Index)[0]) : 0.0;
    }

    float4 Vec4(uint MatIndex, uint Index)
    {
        const FMaterialHeader Header = GetHeader(MatIndex);
        return Index < Header.NumVectors ? loadAligned<16>(reinterpret<float4*>(Data.At(Header.DataWord + Index * 4u))) : float4(0.0);
    }

    uint Texture(uint MatIndex, uint Index)
    {
        const FMaterialHeader Header = GetHeader(MatIndex);
        return Index < Header.NumTextures ? Data.At(Header.TextureWord + Index)[0] : 0u;
    }
};

// Mirror of Lumina::FMaterialCollectionUniforms; one block per parameter collection, shared by materials.
struct FMaterialCollection
{
    float4  Vectors[MAX_COLLECTION_VECTORS];
    float   Scalars[MAX_COLLECTION_SCALARS];
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

// Mirror of Lumina::FSurfaceDescGPU (80 B). kNoSurfaceDesc means the LOD is fixed (skinned, forced, or LODs off).
static const uint kNoSurfaceDesc = NO_SURFACE_DESC_INDEX;

struct FSurfaceDesc
{
    uint  LODMeshletOffset[MESHLET_MAX_LODS];
    uint  LODMeshletCount[MESHLET_MAX_LODS];
    // Mesh-local error per LOD, non-decreasing, so the first one a view cannot accept ends the walk.
    float LODError[MESHLET_MAX_LODS];
    uint  NumLODs;
    uint  _Pad;
}

// Mirror of Lumina::FMeshlet.
struct FMeshlet
{
    uint VertexOffset;
    uint TriangleOffset;
    uint BaseVertex;
    uint PackedCounts;

    property uint VertexCount      { get { return PackedCounts & MESHLET_COUNT_MASK; } }
    property uint TriangleCount    { get { return (PackedCounts >> MESHLET_TRIANGLE_COUNT_SHIFT) & MESHLET_COUNT_MASK; } }
    property uint LODIndex         { get { return (PackedCounts >> MESHLET_LOD_SHIFT) & MESHLET_LOD_MASK; } }
    property bool bShortVertexRefs { get { return ((PackedCounts >> MESHLET_SHORT_REFS_SHIFT) & 1u) != 0u; } }
};

// Built from the float exponent field rather than exp2 so C++ and the shader produce identical bits.
float PositionGridScale(int Exponent)
{
    return asfloat(uint(clamp(Exponent, -126, 127) + 127) << 23);
}

// Twin of the C++ DecodeMeshPosition, bit for bit, so CPU picking and collision agree with what rasterizes.
float3 DecodeMeshPosition(int4 Grid, uint2 Packed)
{
    const float Scale = PositionGridScale(Grid.w);
    const uint  X     = Packed.x & MESH_POSITION_MAX;
    const uint  Y     = ((Packed.x >> MESH_POSITION_BITS) | (Packed.y << (32 - MESH_POSITION_BITS))) & MESH_POSITION_MAX;
    const uint  Z     = (Packed.y >> (2 * MESH_POSITION_BITS - 32)) & MESH_POSITION_MAX;
    return float3(float(Grid.x + int(X)) * Scale, float(Grid.y + int(Y)) * Scale, float(Grid.z + int(Z)) * Scale);
}

// Mirror of Lumina::FMeshletBounds.
struct FMeshletSphere
{
    float3 Center;
    float  Radius;
};

// Mirror of Lumina::FMeshletCone, four SNORM8 bytes in one word since shaders here have no 8-bit types.
struct FMeshletCone
{
    uint Packed;
};

int SignExtendConeByte(uint Packed, uint ByteIndex)
{
    return int(Packed << (24u - 8u * ByteIndex)) >> 24;
}

// xyz is the axis and w the cutoff. Neither is renormalized, the encoder folded that error into w.
float4 DecodeMeshletCone(FMeshletCone C)
{
    const float Scale = 1.0 / float(MESHLET_CONE_SNORM_SCALE);

    return float4(float(SignExtendConeByte(C.Packed, 0u)) * Scale,
                  float(SignExtendConeByte(C.Packed, 1u)) * Scale,
                  float(SignExtendConeByte(C.Packed, 2u)) * Scale,
                  float(SignExtendConeByte(C.Packed, 3u)) * Scale);
}

// Mirrors Lumina::EncodeMeshletCone, rounding the cutoff up so a re-encoded cone never rejects more.
FMeshletCone EncodeMeshletCone(float3 Axis, float Cutoff)
{
    const float Scale = float(MESHLET_CONE_SNORM_SCALE);

    const float3 Clamped   = clamp(Axis, -1.0, 1.0);
    const int3   Quantized = int3(Clamped * Scale + sign(Clamped) * 0.5);
    const float3 Error     = abs(float3(Quantized) / Scale - Clamped);

    const int QCutoff = clamp(int(Scale * (Cutoff + Error.x + Error.y + Error.z) + 1.0),
                              -MESHLET_CONE_SNORM_SCALE, MESHLET_CONE_DISABLED);

    FMeshletCone Out;
    Out.Packed = (asuint(Quantized.x) & 0xFFu)
               | ((asuint(Quantized.y) & 0xFFu) << 8)
               | ((asuint(Quantized.z) & 0xFFu) << 16)
               | ((asuint(QCutoff)     & 0xFFu) << 24);
    return Out;
}

// The cone a meshlet without a usable one carries, matching Lumina::kDisabledMeshletCone.
FMeshletCone DisabledMeshletCone()
{
    FMeshletCone Out;
    Out.Packed = (uint(MESHLET_CONE_SNORM_SCALE) << 16) | (uint(MESHLET_CONE_DISABLED) << 24);
    return Out;
}

// cos(A - B) from the two cosines, which keeps the cone widening off the trig units entirely.
float WidenCutoff(float Cutoff, float CosSpread)
{
    const float SinCutoff = sqrt(saturate(1.0 - Cutoff * Cutoff));
    const float SinSpread = sqrt(saturate(1.0 - CosSpread * CosSpread));

    return Cutoff * CosSpread + SinCutoff * SinSpread;
}

// Mirror of Lumina::FMeshletBonePalette, one meshlet's slice of FMeshletHeader::BoneIndices.
struct FMeshletBonePalette
{
    uint Offset;
    uint Count;
};

// Mirror of Lumina::FMeshletHeaderGPU, with loose distance-field floats since C++ may over-align FVector3 to 16.
struct FMeshletHeader
{
    FMeshlet*       Meshlets;
    FMeshletSphere* Spheres;
    uint*           Vertices;
    uint*           Triangles;
    FMeshletCone*   Cones;

    // A skinned vertex's JointIndices address BonePalettes[meshlet]; BonePaletteCount bounds it.
    FMeshletBonePalette* BonePalettes;
    uint*                BoneIndices;

    FTexture3D DistanceField;        // Invalid when the mesh has no baked field.
    uint       DistanceFieldFlags;   // bit0 = two-sided (unsigned) encoding

    float           DistanceFieldMinX, DistanceFieldMinY, DistanceFieldMinZ;
    float           DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ;
    float           DistanceFieldMaxDistance;

    // Read by the LocalBounds material node, and zero is a degenerate box it substitutes for.
    float           LocalMinX, LocalMinY, LocalMinZ;
    float           LocalMaxX, LocalMaxY, LocalMaxZ;

    // Authoritative bound for every array above, and 0 on the null header, which is why no pointer is null-checked.
    uint            MeshletCount;

    // Zero on a static mesh.
    uint            BonePaletteCount;

    // MESH_VERTEX_STREAM_* bits for the optional static streams below.
    uint            VertexStreamFlags;

    // A static mesh's grid position per shared vertex. See DecodeMeshPosition.
    uint2*          Positions;
    uint*           VertexRefs;
    uint*           VertexUV1s;
    uint*           VertexColors;

    int4            PositionGrid;

    property float3 DistanceFieldMin  { get { return float3(DistanceFieldMinX,  DistanceFieldMinY,  DistanceFieldMinZ);  } }
    property float3 DistanceFieldSize { get { return float3(DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ); } }
    property float3 LocalBoundsMin    { get { return float3(LocalMinX, LocalMinY, LocalMinZ); } }
    property float3 LocalBoundsMax    { get { return float3(LocalMaxX, LocalMaxY, LocalMaxZ); } }
};

// Mirror of Lumina::FInstanceCullEntry (32 B), the cull-hot half only so two slots share a cache line.
struct FInstanceCullEntry
{
    float4 SphereBounds;      // world space
    uint   DrawIDAndFlags;    // same packing as FGPUInstance; carries Active and HasGeometry
    uint   SurfaceDescIndex;
    float  MaxDrawDistance;   // 0 never distance-culls
    uint   LODAndThinning;    // Lumina::PackLODAndThinning

    property int ForcedLODIndex { get { return int(LODAndThinning & 0xFFu) - 1; } }

    // Past this camera distance a foliage instance thins out, 0 never.
    property float ThinningStartDistance { get { return f16tof32(LODAndThinning >> 16u); } }

    bool HasFlag(EInstanceFlags Flag)
    {
        uint Mask = (uint)Flag << 16;
        return (DrawIDAndFlags & Mask) == Mask;
    }

    property uint DrawID { get { return DrawIDAndFlags & 0xFFFFu; } }

    property EInstanceFlags Flags { get { return EInstanceFlags((DrawIDAndFlags >> 16) & 0xFFFFu); } }
}

// Mirror of Lumina::FInstanceStatic (32 B), the cold half that changes on a re-bind and never on a move.
struct FInstanceStatic
{
    // A slot in the header slab, never an address.
    uint            MeshletHeaderSlot;
    FCustomPrimitiveData CustomData;
    uint            MaterialIndex;
    uint            EntityID;
    uint            BoneOffset;
    uint            SkinnedVertexBase;
    // One visibility word per block of its camera LOD, kNoMeshletVisibility for none.
    uint            MeshletVisibilityBase;
    uint            _Pad0;
}

// Word 0 is never handed out, so a zeroed payload reads as an instance with no meshlet visibility.
static const uint kNoMeshletVisibility = 0u;

// Not pre-skinned this frame (over r.Skinning.MaxVertices), so the draw path blends bones inline.
static const uint kNoPreSkinBase = 0xFFFFFFFFu;
static const uint kNoBoneSlice   = 0xFFFFFFFFu;
// No per-frame skinned meshlet bounds, so the cull falls back to bind-pose spheres and stops trusting them.
static const uint kNoSkinnedBounds = 0xFFFFFFFFu;

// Mirror of Lumina::FSkinnedFrameData, indexed by retained slot, with LOD ranges pinned to the pick the pre-skin slice used.
struct FSkinnedFrameData
{
    // The array is never cleared, so a mismatched tag means the slot was not gathered this frame.
    uint FrameTag;
    uint SurfaceMeshletOffset;
    uint SurfaceMeshletCount;
    uint MeshletTotalCount;
    uint SkinnedVertexBase;
    // Slice in the compacted per-frame arena, and kNoBoneSlice blends as identity.
    uint BoneOffset;
    // Folds in the range start, so SkinnedMeshletBounds() is indexed by a mesh-global meshlet index.
    uint SkinnedBoundsBase;

    bool IsFromFrame(uint ExpectedTag) { return FrameTag == ExpectedTag && ExpectedTag != 0u; }
};

// Mirror of Lumina::FTransform3x4. Rows are mathematical rows, since float4x4(v0..v3) fills rows.
struct FTransform3x4
{
    float4 Row0;
    float4 Row1;
    float4 Row2;

    float4x4 ToMatrix()
    {
        return float4x4(Row0, Row1, Row2, float4(0.0, 0.0, 0.0, 1.0));
    }
};

// Mirror of Lumina::FPackedBoneTransform, 3x3 in halves with full-precision translation.
struct FBoneTransform
{
    uint  Rot[5];
    float Tx;
    float Ty;
    float Tz;
};

// Stamped with the writing frame, so a stale slot past a capacity bound rejects instead of drawing.

#define MESHLET_DRAW_INDEX_MASK  ((1u << MESHLET_DRAW_INDEX_BITS) - 1u)
#define MESHLET_DRAW_TAG_INVALID 0u

struct FMeshletDraw
{
    uint InstanceID;          // Into Instances()
    uint PackedIndexAndTag;   // [19:0] index into the mesh's meshlet array, [31:20] frame tag

    property uint MeshletIndex { get { return PackedIndexAndTag & MESHLET_DRAW_INDEX_MASK; } }
    property uint Tag          { get { return PackedIndexAndTag >> MESHLET_DRAW_INDEX_BITS; } }

    bool IsFromFrame(uint ExpectedTag) { return Tag == ExpectedTag && ExpectedTag != MESHLET_DRAW_TAG_INVALID; }

    static uint Pack(uint MeshletIndex, uint FrameTag)
    {
        return (FrameTag << MESHLET_DRAW_INDEX_BITS) | (MeshletIndex & MESHLET_DRAW_INDEX_MASK);
    }
};

// Count is load-bearing, since a partial block's spare lanes would address the next LOD.
struct FMeshletBlock
{
    uint InstanceID;          // slot in the visible-instance buffer
    // Base is mesh-global with the view's LOD pick already folded in.
    uint PackedBaseAndCount;

    static uint Pack(uint MeshletBase, uint Count)
    {
        return (MeshletBase & MESHLET_DRAW_INDEX_MASK) | ((Count & 63u) << MESHLET_DRAW_INDEX_BITS);
    }

    property uint MeshletBase { get { return PackedBaseAndCount & MESHLET_DRAW_INDEX_MASK; } }
    property uint Count       { get { return (PackedBaseAndCount >> MESHLET_DRAW_INDEX_BITS) & 63u; } }
};

// A bucket that fits its whole demand never steps an instance down a LOD, so its blocks can be split by cull phase.
bool BucketFitsDemand(uint DrawDemand, uint DrawCapacity, uint BlockDemand, uint BlockCapacity)
{
    return DrawDemand <= DrawCapacity && BlockDemand <= BlockCapacity;
}

// One (view, draw) pair's slice of the cull arenas.
struct FRenderBucket
{
    uint DrawBase;
    uint DrawCapacity;
    uint DrawCursor;
    uint BlockBase;
    uint BlockCapacity;
    uint BlockCursor;
    // Blocks are sparse across the arena, so a workgroup cannot read its bucket from the block itself.
    uint CullWorkBase;
    // Meshlets BuildMeshletBlocks has promised the draw region, so a run is only built when its worst case fits.
    uint DrawBudgetCursor;
    // What CullInstances asked for before BuildDrawPrefix fitted the bucket into the arena.
    uint DrawDemand;
    uint BlockDemand;
    // Blocks the late pass appended from the end of the block region, in a two-phase bucket that fits its whole demand.
    uint BlockBackCount;
    // Relative to DrawBase and indexed by MESHLET_SLICE_*, because both VisBuffer phases share cull view 0.
    uint SliceBase[MESHLET_SLICE_COUNT];
    uint SliceCount[MESHLET_SLICE_COUNT];
    // Read as the draw count by vkCmdDrawMeshTasksIndirectCountEXT, so it must stay the last field.
    uint SubDrawCount[MESHLET_SLICE_COUNT];
};

// Mirror of Lumina::FGPUInstance.
struct FGPUInstance
{
    FTransform3x4           Transform;
    float4                  SphereBounds;
    // A slot in the header slab, never an address, so a dead mesh resolves to the null header instead of dangling.
    uint                    MeshletHeaderSlot;
    // Copied from FInstanceStatic, so the meshlet cull reads it without the retained payload.
    uint                    MeshletVisibilityBase;

    // Explicitly zeroed because Slang's synthesized default initializer skips private members.
    private uint            DrawIDAndFlags = 0u;
    // The primary camera's LOD, used verbatim by every view when the instance cannot re-select.
    uint                    SurfaceMeshletOffset;
    uint                    SurfaceMeshletCount;
    FCustomPrimitiveData    CustomData;

    uint                    BoneOffset;
    uint                    MaterialIndex;
    uint                    EntityID;
    // Into PreSkinnedVertices(), 0 for static and kNoPreSkinBase when the entity lost the pre-skin budget.
    uint                    SkinnedVertexBase;
    // Into SurfaceDescs so a cull view can re-select the LOD, and kNoSurfaceDesc pins SurfaceMeshletOffset/Count.
    uint                    SurfaceDescIndex;
    // End of the last LOD, the bound on a mesh-global meshlet index.
    uint                    MeshletTotalCount;
    // Into InstanceVisibility, keyed by the retained slot since the compacted slot changes every frame.
    uint                    RetainedSlot;
    uint                    _Pad0[3];

    property float4x4 ModelMatrix { get { return Transform.ToMatrix(); } }

    // Matches Lumina::PackDrawIDAndFlags, DrawID in the low 16 bits and flags in the high 16.
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

    // Pointer forms of the three accessors above, which through a pointer would load all 128 bytes.
    static bool HasFlag(FGPUInstance* P, EInstanceFlags Flag)
    {
        const uint Mask = (uint)Flag << 16;
        return (P.DrawIDAndFlags & Mask) == Mask;
    }

    static EInstanceFlags GetFlags(FGPUInstance* P) { return EInstanceFlags((P.DrawIDAndFlags >> 16) & 0xFFFFu); }
    static uint           GetDrawID(FGPUInstance* P) { return P.DrawIDAndFlags & 0xFFFFu; }
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

// 160 B, must match FGPUReflectionProbe. Half-extents are baked into WorldToProbe, so the volume is a unit shape.
struct FGPUReflectionProbe
{
    float4x4    WorldToProbe;
    float4x4    ProbeToWorld;
    float4      CapturePosition;   // xyz = world capture origin, w = 1 once this probe has been baked
    float4      Params;            // x=brightness, y=shape, z=array slice, w=blend distance in meters

    property float  Brightness   { get { return Params.x; }}
    property bool   IsSphere     { get { return Params.y > 0.5; }}
    property uint   Slice        { get { return (uint)Params.z; }}
    property float  BlendDistance{ get { return Params.w; }}
    // An unbaked probe would consume influence and return black, so it is skipped.
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

// 64 B, must match FGPUSplineSample. Entries are uniform in distance, which lets a lookup be a divide.
struct FGPUSplineSample
{
    float3 Position; float DistanceAlong;
    float3 Tangent;  float Key;    // curve key (0..NumSegments) this sample landed on
    float3 Up;       float Roll;   // degrees
    float3 Scale;    float _Pad;
};

static const uint SPLINE_FLAG_CLOSED_LOOP = 1u;

// Points and samples are slices of two shared arrays. 160 B, must match FGPUSpline.
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
    // Must match SSplineComponent::GetNumSegments, where fewer than two points is zero segments.
    property uint NumSegments  { get { return PointCount < 2u ? 0u : (IsClosedLoop ? PointCount : PointCount - 1u); }}
};

static const float kProbeShapeBox    = 0.0;
static const float kProbeShapeSphere = 1.0;

static const uint WIDGET_FLAG_BILLBOARD = 1u;

// Hot per-light data sized to one 64 B cache line, with cold shadow data in FLightShadowData.
struct FLight
{
    // Position and Radius share the first 16 bytes, so the clustered loop culls on one load.
    float3 Position;
    float  Radius;

    float3 Direction;   // points from the surface toward the light
    uint   Color;

    float  Intensity;
    float  Falloff;
    float2 Angles;

    uint   Flags;
    int    ShadowDataIndex;

    // Only meaningful when Flags has ELightFlags::Volumetric.
    float  VolumetricIntensity;
    float  VolumetricScatteringRadius;   // soft-core source radius (fraction of Radius) for spot/point fog
};

// Quantized minimum roughness, in the top byte of FLight.Flags so the struct stays one cache line.
static const uint LIGHT_MIN_ROUGHNESS_SHIFT = 24u;

float GetLightMinRoughness(FLight Light)
{
    return float(Light.Flags >> LIGHT_MIN_ROUGHNESS_SHIFT) * (1.0 / 255.0);
}

// Cold shadow data, one entry per shadow-casting light, capped at MAX_SHADOWS.
struct FLightShadowData
{
    float4x4     ViewProjection[6];
    FLightShadow Shadow[6];
};

// Mirror of Lumina::FLightFunction, indexed by the light's atlas tile.
struct FLightFunction
{
    // Right.w is the projection scale, 1/tan of a spot's cone or 1/extent in meters for the sun.
    float4 Right;
    float4 Up;
};

struct FLightData
{
    uint   bHasIBL;       // 0 falls back to flat skylight ambient
    float3 SkySunDirection; // the true sun for the sky, which moonlight leaves behind at night
    float3 SunDirection;   // to-light for surface lighting and shadows (== Lights[0].Direction)
    uint   bHasSun;
    float4 CascadeSplits;
    float4 CascadeRadii;
    // Pixels per side, since progressive packing gives each cascade a different texel size.
    float4 CascadeResolutions;
    // Ortho depth range of each cascade, which turns a shadow-map NDC z delta into world units.
    float4 CascadeDepthRanges;
    // Mirrors FSceneLightData::ShadowParams (normal bias, depth bias, PCF radius in cascade-0 texels, cascade blend).
    float4 ShadowParams;
    // Far-cascade fade fraction, PCF tap count, active cascade count, and 1 when the last cascade is the far one.
    float4 ShadowParams2;
    float4 AmbientLight;
    // Each carries its own live count, so no caller has to remember a separate bound.
    GPUSpan<FLight>           Lights;
    GPUSpan<FLightShadowData> Shadows;
    GPUSpan<FLightFunction>   LightFunctions;
    FTexture2D                LightFunctionAtlas;
    uint                      _LightFunctionPad[3];
};

struct FCluster
{
    float4 MinPoint;
    float4 MaxPoint;
};

struct FFrustum
{
    float4 Planes[6];
};

// Per-view cull descriptor, one per render view; draw-list regions are packed per (view, draw) on the GPU.
struct FCullView
{
    float4 FrustumPlanes[6];   // 96B
    float4 ViewOriginAndFlags; // 16B, xyz=origin (camera / light pos), w=asuint(flags)
    uint   CascadeIndex;       // Only meaningful when Flags has CULL_VIEW_FLAG_CASCADE
    float  MinBoundsDiameter;  // world units, 0 is off
    // Pixels a unit of world error covers at unit distance, over the pixel error the view tolerates; 0 pins LOD 0.
    float  LODErrorScale;
    uint   _Pad0;

    property float3 ViewOrigin { get { return ViewOriginAndFlags.xyz; } }
    property uint   Flags      { get { return asuint(ViewOriginAndFlags.w); } }

    bool HasFlag(uint Flag) { return (Flags & Flag) == Flag; }
};

struct FCullData
{
    // The cull camera, separate from the render camera so freezing the cull cannot move the picture.
    float4   CullCameraPosition;
    float4x4 CullCameraView;
    float4x4 CullCameraProjection;

    FFrustum Frustum;
    FFrustum ShadowFrustum;
    // Tight per-cascade caster volume, not the nested ortho box that would put every caster in every cascade.
    FFrustum CascadeFrustum[NUM_CASCADES];
    // Cascade transforms that produced the pyramid's current contents (last frame's).
    float4x4 CascadeHZBViewProjection[NUM_CASCADES];
    float4   CascadeHZBTile[NUM_CASCADES];      // xy = pyramid-UV offset of the tile, zw = UV scale
    float4   CascadeHZBNdcScale[NUM_CASCADES];  // xyz = NDC units per world unit
    // This frame's transforms for the phase-2 re-test, never to be mixed with the pair above.
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
    // Tag the meshlet draw list is stamped with this frame, never 0.
    uint       MeshletDrawTag;
    uint       DebugMode;
    uint       MeshletDrawListCapacity;   // entries in MeshletDrawList(); an index bound, not a frame total
    FTexture2D DepthPyramid;
    // Max-reduced since the cascade atlas is standard-Z, and invalid until the first cascade raster is pyramided.
    FTexture2D CascadePyramid;
    uint       bCascadeHZBValid;
    uint       bCascadeHZBMidValid;      // same, for the mid-frame pyramid phase 2 reads
    float      CascadePyramidWidth;
    float      CascadePyramidHeight;
    uint       CascadePyramidMipCount;
    uint       BoneNum;                  // entries in Bones(); an index bound, not a frame total
    // The cull camera's clip range, which under a frozen cull is not the live view's.
    float    CullNearPlane;
    float    CullFarPlane;
    // Holds the struct at a 16-byte multiple, which the members after it in FSceneGlobalData rely on.
    uint     _CullPad0;
    uint     _CullPad1;
    uint     _CullPad2;
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
#define DEBUG_MODE_SUN_FAR_SHADOW       14u
#define DEBUG_MODE_GTAO                 15u
#define DEBUG_MODE_MATERIAL_ID          16u
#define DEBUG_MODE_TRIANGLE_ID          17u
// LAYER_COUNT also makes the translucent BasePixelPass write 1.0 into accum.a as a raw fragment count.
#define DEBUG_MODE_OIT_ACCUM_COLOR      18u
#define DEBUG_MODE_OIT_TRANSMITTANCE    20u
#define DEBUG_MODE_OIT_LAYER_COUNT      21u
#define DEBUG_MODE_PROBE_INFLUENCE      22u
#define DEBUG_MODE_PROBE_RADIANCE       23u
// Subject to the per-model GBuffer borrow, so unstored pixels show kDebugNotStored (SurfaceShading.slang).
#define DEBUG_MODE_SPECULAR             24u
#define DEBUG_MODE_SHADING_MODEL        25u
#define DEBUG_MODE_CLEARCOAT            26u
#define DEBUG_MODE_CLEARCOAT_ROUGHNESS  27u
#define DEBUG_MODE_SELF_SHADOW          28u
// Composites over lit shading instead of replacing it, deferred lane only since it needs VisBuffer barycentrics.
#define DEBUG_MODE_WIREFRAME_OVERLAY    29u
#define DEBUG_MODE_VELOCITY             30u

// Shading lanes per 2x2 quad that survived to the VisBuffer; the micro-triangle cost readout.
#define DEBUG_MODE_QUAD_EFFICIENCY      31u

// Global scale on every material's POM settings. Matches FParallaxSettings in SceneRenderTypes.h.
struct FParallaxSettings
{
    float SampleScale;         // scales the material's Min/Max sample counts; <= 0 disables POM outright
    float LODBias;             // added to the material's LOD threshold; negative fades POM out nearer
    float ShadowSampleScale;   // scales self-shadow samples; 0 disables self-shadowing globally
    float _Pad0;
};

// One medium definition shared by the froxel passes and the analytic composite.
struct FExponentialHeightFogParams
{
    float4 InscatteringColor;   // rgb = fog albedo, w = density at base height
    float4 HeightParams;        // x = falloff, y = baseHeight, z = startDistance, w = maxOpacity
    float4 DirectionalColor;    // rgb = sun inscatter tint, w = exponent
    float4 VolumetricParams;    // x = scatterIntensity, y = anisotropy, z = froxel range, w = dirStartDistance
    // See Fog_MultiScatterPhaseShadow for what the four channels drive.
    float4 MultiScatterParams;
    // x = noise strength (0 disables), y = inverse world scale, z = octave count, w = per-octave gain.
    float4 NoiseParams;
    // xyz = wind velocity in world units per second, w = distance the noise has faded out by.
    float4 NoiseWind;
};

// One artist-placed density volume. WorldToVolume maps world space into the unit cube or sphere.
struct FFogVolume
{
    float4x4 WorldToVolume;
    float4   Albedo;      // rgb = albedo, w = extinction added at full strength
    float4   Emissive;    // rgb = emissive in-scatter, w = unused
    float4   Params;      // x = shape (0 box, 1 sphere), y = edge softness, z = scatter intensity, w unused
};

// Matches GFogMaxVolumes on the CPU side; the loop is bounded so a malformed count cannot run away.
static const uint FOG_MAX_VOLUMES = 32;

// Repeated here so a malformed upload cannot run the octave loop unbounded in the per-light path.
static const int FOG_MAX_MULTISCATTER_OCTAVES = 4;

struct FSceneGlobalData
{
    FCameraView       CameraView;
    uint4             ScreenSize;
    uint4             GridSize;
    float             Time;
    float             PrevTime;
    float             DeltaTime;
    float             NearPlane;
    float             FarPlane;
    // This frame's projection jitter in UV units, so motion vectors measure from where a surface really is.
    float             TemporalJitterU;
    float             TemporalJitterV;
    // Added to every material texture's mip level, below zero while the view renders under the display resolution.
    float             TextureMipBias;
    FGTAOSettings     GTAOSettings;
    FCullData         CullData;
    FParallaxSettings ParallaxSettings;

    // Subsample index 0 or 1 that decorrelates screen-space noise. Stays 0 unless T2x is resolving.
    uint              TemporalPhase;

    // Opaque scene depth, for the screen-space traces that run after the depth pass.
    FTexture2D              SceneDepth;
    // This view's decal layers, ~0u when no decal rendered.
    FTexture2D DBufferA;
    FTexture2D DBufferB;
    FTexture2D DBufferC;
    uint       _PadFogParams0;
    uint       _PadFogParams1;
    uint       _PadFogParams2;

    // The translucent passes fog themselves, since the composite runs first and sees only opaque depth.
    FExponentialHeightFogParams FogParams;

    FTexture3D FogIntegrated;   // Invalid when there is no froxel volume this frame.
    uint       FogGridZ;
    float      FogNearPlane;
    float      FogRange;

    uint       bFogEnabled;
    uint       FogFarShaftSteps;
    float      FogFarShaftDistance;
    FTexture2D FogCloudShadow;        // Invalid when there is no cloud shadow this frame.

    float2            FogCloudShadowCenter;
    float             FogCloudShadowExtent;
    // The additive emission layer decals write, ~0u when no decal rendered.
    FTexture2D              DBufferD;
};

// Matches Lumina::PackTangent, and the bitangent is cross(N, tangent) * sign.
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

// Unlike PackNormal, keeps the oct coords as floats so they survive alpha blending.
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

// Cofactor-form inverse-transpose. The indexing is row-wise, and reading columns yields inverse(A).
float3x3 CalculateNormalMatrix(float4x4 ModelMatrix)
{
    float3 A0 = ModelMatrix[0].xyz;
    float3 A1 = ModelMatrix[1].xyz;
    float3 A2 = ModelMatrix[2].xyz;

    float3 Cof0 = cross(A1, A2);
    float3 Cof1 = cross(A2, A0);
    float3 Cof2 = cross(A0, A1);

    float InvDet = 1.0 / dot(A0, Cof0);   // A0 . (A1 x A2) = det(A^T) = det(A)

    return float3x3(Cof0 * InvDet, Cof1 * InvDet, Cof2 * InvDet);
}

float3 GetInstanceScale(float4x4 ModelMatrix)
{
    return float3(
        length(mul(ModelMatrix, float4(1.0, 0.0, 0.0, 0.0)).xyz),
        length(mul(ModelMatrix, float4(0.0, 1.0, 0.0, 0.0)).xyz),
        length(mul(ModelMatrix, float4(0.0, 0.0, 1.0, 0.0)).xyz));
}

// Exact inverse of an affine transform, cofactor form so a degenerate scale stays finite.
float4x4 MakeInstanceWorldToLocal(float4x4 ModelMatrix)
{
    // CalculateNormalMatrix returns (A^-1)^T for the upper-left 3x3 A, so its transpose is A^-1.
    float3x3 InvRotScale = transpose(CalculateNormalMatrix(ModelMatrix));

    float3 Translation = float3(ModelMatrix[0].w, ModelMatrix[1].w, ModelMatrix[2].w);
    float3 InvTranslation = -mul(InvRotScale, Translation);

    return float4x4(
        float4(InvRotScale[0], InvTranslation.x),
        float4(InvRotScale[1], InvTranslation.y),
        float4(InvRotScale[2], InvTranslation.z),
        float4(0.0, 0.0, 0.0, 1.0));
}

// The step VogelDiskSample takes between consecutive taps, 2 * PI * (1 - 1 / PHI).
static const float kVogelGoldenAngle = 2.399963229728653;

// Turns a tap's unit direction by a precomputed (cos, sin), so a loop pays one sincos instead of one per tap.
float2 RotateVogel(float2 Dir, float2 Step)
{
    return float2(Dir.x * Step.x - Dir.y * Step.y, Dir.x * Step.y + Dir.y * Step.x);
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

// Triangular-PDF dither (Gjol/Playdead) in a fixed pattern, since a per-frame one flickers wherever a later pass quantizes coarsely.
float3 ScreenSpaceDither(float2 ScreenPos)
{
    float3 S0 = float3(dot(float2(171.0, 231.0), ScreenPos.xy));
    float3 S1 = float3(dot(float2(113.0, 271.0), ScreenPos.xy + 13.7));
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

// Rec. 709 luma, the single luminance definition for the engine.
float Luminance(float3 C)
{
    return dot(C, float3(0.2126, 0.7152, 0.0722));
}