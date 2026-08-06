#pragma once

static const float PHI    = 1.618033988749895;
static const float PI     = 3.1415926535897932384626433832795;
static const float TWO_PI = 6.28318530718;
static const float INV_PI = 0.31830988618;


#define LIGHT_INDEX_MASK 0x1FFFu
#define LIGHTS_PER_UINT  2
#define LIGHTS_PER_CLUSTER 100
#define SHADOW_SAMPLE_COUNT 8
// Upper bound for the directional cascade's runtime-tunable tap count
// (SDirectionalLightComponent::ShadowSampleCount, forwarded via ShadowParams2.y).
#define SHADOW_MAX_SAMPLE_COUNT 16

#define INDEX_NONE -1

#define COL_R_SHIFT 0
#define COL_G_SHIFT 8
#define COL_B_SHIFT 16
#define COL_A_SHIFT 24
#define COL_A_MASK  0xFF000000

#define MAX_LIGHTS      8192
#define MAX_SHADOWS     256
#define NUM_CASCADES    4
#define MAX_CULL_VIEWS  128

// Bits inside FCullView.Flags, gating which culling stages run for the view.
// PhaseLate views re-test last-frame's HZB-occluded defer list against the rebuilt HZB.
#define CULL_VIEW_FLAG_FRUSTUM          BIT(0)
#define CULL_VIEW_FLAG_CONE             BIT(1)
#define CULL_VIEW_FLAG_OCCLUSION        BIT(2)
#define CULL_VIEW_FLAG_DISTANCE         BIT(3)
#define CULL_VIEW_FLAG_CAST_SHADOW_ONLY BIT(4)
#define CULL_VIEW_FLAG_SUN_ALIGNED      BIT(5)
#define CULL_VIEW_FLAG_PHASE_LATE       BIT(6)
#define CULL_VIEW_FLAG_CASCADE          BIT(7)

// CullMeshlets phase: 0 walks all instance/meshlet pairs (emit or defer occluded);
// 1 walks only the defer list and re-tests against the freshly-built HZB.
#define CULL_PHASE_EARLY 0u
#define CULL_PHASE_LATE  1u
// Third phase, run after the cascade atlas is rastered and its pyramid rebuilt. Separate from
// CULL_PHASE_LATE because the two pyramids are rebuilt at different points in the frame, so they
// cannot share a dispatch. Both walk the same defer list and filter on FMeshletDeferred::DeferTag.
#define CULL_PHASE_CASCADE_LATE 2u

// FMeshletDeferred::DeferTag value meaning "the camera pyramid deferred this"; any other value is a
// cascade index. Cascades are bounded by NUM_CASCADES, so ~0u can never collide with a real one.
#define kDeferTagCamera 0xFFFFFFFFu

#define MAX_SCALARS  24
#define MAX_VECTORS  24
#define MAX_TEXTURES 24

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

// Mirror of Lumina::FMeshletVertex. Position: full mesh-local float3 (exact, gap-free).
// Tangent: 15-15 octahedral + 1-bit handedness (see UnpackTangent).
struct FVertex
{
    float3  Position;
    uint    Normal;
    uint    Tangent;
    uint    UV;
    uint    Color;
};

struct FSkinnedVertex
{
    float3  Position;
    uint    Normal;
    uint    Tangent;
    uint    UV;
    uint    Color;
    uint    JointIndices;
    uint    JointWeights;
};

// Mirror of Lumina::FPreSkinnedVertex. Written by Skinning.slang, read by every draw VS as the
// only vertex source. Position is full-precision (skinning breaks meshlet quantization); all-scalar
// for a tight 28 B stride.
struct FPreSkinnedVertex
{
    float   Px;
    float   Py;
    float   Pz;
    uint    Normal;     // PackNormal
    uint    Tangent;    // PackTangent
    uint    UV;         // carried through from the source vertex (skin-invariant)
    uint    Color;
};

struct FVertexData
{
    float3 Position;
    float3 Normal;
    float4 Tangent;    // xyz: tangent direction; w: handedness sign for B = cross(N,T)*w
    float2 UV;
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
    // Height-field self-occlusion against the SUN specifically (ParallaxOcclusion.slang), so it modulates
    // only the sun's contribution -- not ambient (that is AmbientOcclusion's job) and not the punctual
    // lights, which were never traced. 1 = unshadowed.
    float  SelfShadow;
};

// Neutral surface. EVERY producer of FMaterialPixelInputs must start from this and then override, because
// the struct has several independent authors -- the material graph's output node, and the hand-written
// default/terrain materials built as strings in Material.cpp. Assigning fields one by one at each site
// means adding a field here silently leaves it uninitialized at the sites that were not updated, which
// reads as garbage lighting rather than as a compile error. Defaults live here so there is one place.
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
    M.Opacity          = 1.0;
    M.SelfShadow       = 1.0;
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

// GTAO tuning; matches C++ FSSAOSettings (SceneRenderTypes.h). Slice directions come from
// per-pixel interleaved gradient noise in the shader, no CPU kernel.
struct FSSAOSettings
{
    float  Radius;
    float  Intensity;
    float  Power;
    float  Bias;             // reserved (unused by GTAO)
    uint   AOTextureIndex;    // bindless SRV the base pass samples; ~0u = no SSAO for this view
    uint   _Pad0;
    uint   _Pad1;
    uint   _Pad2;
};

static const uint MatFlag_None        = 0;
static const uint MatFlag_Masked      = BIT(0);
static const uint MatFlag_Translucent = BIT(1);
static const uint MatFlag_Additive    = BIT(2);
static const uint MatFlag_Unlit       = BIT(3);

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

// Meshlet limits must match Lumina::MESHLET_MAX_VERTICES / MAX_TRIANGLES / MAX_MESH_LODS.
#define MESHLET_MAX_VERTICES  64
#define MESHLET_MAX_TRIANGLES 124
#define MESHLET_VERTICES_PER_DRAW (MESHLET_MAX_TRIANGLES * 3)  // 372
#define MESHLET_MAX_LODS      6
// Matches Lumina::MAX_SHADOW_LOD: shadow views never take a LOD past this, however far the bias pushes.
#define MESHLET_MAX_SHADOW_LOD 3
// Matches Lumina::MAX_COARSE_SHADOW_LOD: the cap past ShadowCoarseLODDistance, where one cascade texel is
// wide enough that a sloppy LOD's holes stay sub-texel.
#define MESHLET_MAX_COARSE_SHADOW_LOD 5

// Mirror of Lumina::FSurfaceDescGPU (80 B). Per-surface LOD table, interned per distinct resolved
// surface, so a cull view can pick the LOD the CPU gather used to pick -- per view, from its own origin.
//
// FGPUInstance::SurfaceDescIndex carries this sentinel when the instance's LOD is fixed and must not be
// re-selected: skinned instances (CPU-fed, and their pre-skin blocks are built for one specific LOD),
// an authored ForcedLODIndex, or LODs disabled scene-wide.
static const uint kNoSurfaceDesc = 0xFFFFFFFFu;

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
};

// Mirror of Lumina::FMeshletBounds. Sphere + cone drive culling.
struct FMeshletBounds
{
    float3 Center;
    float  Radius;
    float3 ConeApex;
    float  ConeCutoff;
    float3 ConeAxis;
    float  _Pad0;
};

// Mirror of Lumina::FMeshletHeaderGPU. Positions are full float3 in the vertex buffer -- no per-LOD grid.
//
// The distance-field block is loose floats, not a float3, because scalar layout aligns a float3 to 4
// bytes while the C++ side is free to over-align FVector3 to 16. Keep this member-for-member identical
// to FMeshletHeaderGPU (MeshData.h), which static_asserts the 80-byte total.
struct FMeshletHeader
{
    FMeshlet*       Meshlets;
    FMeshletBounds* Bounds;
    uint*           Vertices;
    uint*           Triangles;

    uint            DistanceFieldIndex;     // gTextures3D[] slot, or kInvalidDistanceField
    uint            DistanceFieldFlags;     // bit0 = two-sided (unsigned) encoding

    float           DistanceFieldMinX, DistanceFieldMinY, DistanceFieldMinZ;
    float           DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ;
    float           DistanceFieldMaxDistance;

    uint            _DFPad0;
    uint            _DFPad1;
    uint            _DFPad2;

    property float3 DistanceFieldMin  { get { return float3(DistanceFieldMinX,  DistanceFieldMinY,  DistanceFieldMinZ);  } }
    property float3 DistanceFieldSize { get { return float3(DistanceFieldSizeX, DistanceFieldSizeY, DistanceFieldSizeZ); } }
};

// Mirror of Lumina::FInstanceCullEntry (32 B). The retained scene's CULL-HOT half: everything
// CullInstances reads about a slot it is about to REJECT, and nothing else, so two slots share a cache
// line. What a SURVIVOR needs lives in the parallel transform array and FInstanceStatic, neither of
// which is loaded until this entry says the instance is visible.
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
    FMeshletHeader*      MeshletHeader;
    FCustomPrimitiveData CustomData;   // one packed uint; same as FGPUInstance's
    uint            MaterialIndex;
    uint            EntityID;
    uint            BoneOffset;
    uint            SkinnedVertexBase;
    uint            ShadowSkinnedVertexBase;
}

// FGPUInstance::SkinnedVertexBase sentinel: this entity was not pre-skinned this frame (it fell
// outside r.Skinning.MaxVertices), so the draw path blends bones inline instead. The CPU reserves
// the value, so a legitimately wrapped base can never collide with it.
static const uint kNoPreSkinBase = 0xFFFFFFFFu;

// Mirror of Lumina::FSkinDescriptor. One per rendered-LOD meshlet; drives the skinning dispatch.
struct FSkinDescriptor
{
    FMeshletHeader* MeshletHeader;
    uint            BoneOffset;
    uint            SkinnedVertexBase;  // combined: compacted base - vertex span start
    uint            MeshletIndex;       // index into Header.Meshlets (one descriptor per meshlet)
    uint            Pad;
};

// Mirror of Lumina::FTransform3x4: an affine matrix with the always-(0,0,0,1) last row dropped.
// Blends linearly like the matrix; transform a point by explicit row dot products.
//
// The rows are the MATHEMATICAL rows, so ToMatrix below rebuilds the exact float4x4 the CPU held.
// Verified, not assumed: Slang stores float4x4 ColMajor (element (r,c) at float index 4*c+r, identical
// to Lumina::TMat), and float4x4(v0,v1,v2,v3) fills ROWS -- a probe of the ctor plus mul(M, e0)
// constant-folded to column 0 of a row-filled matrix.
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

// Historical name, kept for the skinning path that is written in terms of bones.
typealias FBoneTransform = FTransform3x4;

// One entry per surviving meshlet emitted by MeshletCull.slang.
// Draw-list entry, stamped with the frame that wrote it.
//
// The bounds every consumer used to check were allocation CAPACITIES, which makes them fault-safe but not
// correctness-safe: a slot inside the allocation that this frame never wrote reads as a perfectly plausible
// instance from an earlier frame, and the resolve happily rasterizes it. That is exactly how a draw-count
// bug surfaced as wrong geometry rather than as nothing. The tag makes "not written this frame" detectable,
// so a stale entry is a clean reject instead of silent garbage.
//
// The index is MESH-GLOBAL: it addresses MeshletHeader.Meshlets/Bounds directly, across every LOD, rather
// than being relative to one LOD's range. That is what lets each view pick its OWN LOD -- the entry no
// longer means anything different depending on which range the reader resolves it against, so a capture
// camera's draw and the primary's can sit side by side in the same list.
//
// 20 bits of index (a mesh's meshlet array, all LODs together, never has a million entries -- interning
// clamps it) leaves 12 for the tag. The CPU never issues tag 0, and a freshly allocated draw list is
// zeroed, so untouched memory always fails the check.
#define MESHLET_DRAW_INDEX_BITS  20u
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

// Camera meshlets the early pass deferred via Hi-Z; the late pass re-tests
// against the rebuilt HZB. DrawID is captured at defer time because the
// late pass increments uIndirectArgs[...DrawID].InstanceCount.
struct FMeshletDeferred
{
    uint InstanceID;
    uint MeshletIndex;      // mesh-global, same meaning as FMeshletDraw's
    uint DrawID;
    // Which Hi-Z deferred this, and so which one re-tests it and where it emits: kDeferTagCamera for the
    // camera pyramid, otherwise the cascade index. One meshlet can be occluded in cascade 0 and visible
    // in cascade 1, so a (meshlet, cascade) pair gets its own entry rather than one entry per meshlet.
    uint DeferTag;
};

// Mirror of Lumina::FGPUInstance.
struct FGPUInstance
{
    // 3x4, not 4x4: an instance transform is always affine. Read it through the ModelMatrix property
    // below, which expands it -- that is why every consumer of this struct was untouched by the change.
    FTransform3x4           Transform;
    float4                  SphereBounds;
    FMeshletHeader*         MeshletHeader;

    // Explicitly zeroed: Slang's synthesized default initializer skips private members, so
    // (FGPUInstance)0 left the packed draw id and flags holding whatever was on the stack -- and the
    // accessors below read both out of this one word. That is the kind of value a guard path hands
    // back after refusing to resolve something, which is exactly when it must not be garbage.
    private uint            DrawIDAndFlags = 0u;
    // Meshlet range of the DEFAULT surface LOD -- the one picked from the primary camera. Used verbatim by
    // any view that cannot re-select (SurfaceDescIndex == kNoSurfaceDesc: skinned, forced-LOD, LODs off),
    // and as the reference the per-view pick starts from. Not what the raster resolves against any more:
    // the draw entry carries a mesh-global meshlet index precisely because the LOD is per view.
    uint                    SurfaceMeshletOffset;
    uint                    SurfaceMeshletCount;
    // Shadow LOD's meshlet range. Shadow views keep using this pick rather than re-selecting per view:
    // the cascades are fit to the PRIMARY camera, so the LOD that matches their texel pitch is a function
    // of the primary camera's distance, not of the light's position.
    uint                    ShadowMeshletOffset;
    uint                    ShadowMeshletCount;
    FCustomPrimitiveData    CustomData;

    // Full 32-bit index into the global bone-matrix buffer (no longer packed with the
    // material index), so the scene is not capped at 64k total bones.
    uint                    BoneOffset;
    uint                    MaterialIndex;
    uint                    EntityID;
    // Bases into PreSkinnedVertices() for this instance's surface-LOD and shadow-LOD meshlet blocks
    // (skinning compute writes, draw VS reads). Equal when both resolved to the same block. 0 for
    // static instances, kNoPreSkinBase when the entity lost the pre-skin budget and the draw path
    // must blend bones itself.
    uint                    SkinnedVertexBase;
    uint                    ShadowSkinnedVertexBase;
    // Into SurfaceDescs, so a cull view can re-select this instance's LOD from its OWN origin.
    // kNoSurfaceDesc means "not selectable": use SurfaceMeshletOffset/Count as they stand.
    uint                    SurfaceDescIndex;
    // The meshlet cull's per-instance thread domain: max over (every non-shadow view's picked surface LOD
    // count, the shadow LOD count). Has to be a stored value rather than re-derived, because the prefix
    // scan indexes into the same domain and the two must agree exactly.
    uint                    MeshletWalkCount;
    // Entries in this mesh's meshlet array (end of the last LOD). The bound on a mesh-global meshlet index,
    // and the only bound the raster resolve needs now that the index is not LOD-relative.
    uint                    MeshletTotalCount;

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

// One local reflection probe. 160 B, must match FGPUReflectionProbe in SceneRenderTypes.h.
// Extraction bakes the half-extents into WorldToProbe, so the influence volume is the unit box
// [-1,1]^3 (Box) or the unit sphere (Sphere) in probe space. Sampling helpers live in
// Includes/ReflectionProbe.slang; this declaration sits here only because FSceneRoot points at it.
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
    // A probe waiting in the bake queue owns a slice but has nothing in it. It must be skipped
    // rather than sampled-as-black: consuming influence and returning black would darken the
    // reflection instead of leaving the sky to cover that pixel.
    property bool   IsBaked      { get { return CapturePosition.w > 0.5; }}
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
    // x = far-cascade distance-fade fraction; y = directional PCF tap count; zw reserved.
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

// Per-view cull descriptor. One per render view (main camera, each cascade,
// each point face, each spot). Each view owns a slice of MeshletDrawList /
// IndirectArgs via IndirectArgsOffset; draw-list regions are packed per (view, draw) on the GPU.
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
    // Shadow LOD inputs. They live here rather than in one pass's push constants because BOTH the
    // instance cull (which reserves draw-list room) and the meshlet cull (which walks it) now make the
    // per-cascade pick, and a pick that disagrees between them drops meshlets. Took _CullPad1.
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
#define DEBUG_MODE_SSAO                 15u
#define DEBUG_MODE_MATERIAL_ID          16u
#define DEBUG_MODE_TRIANGLE_ID          17u
// OIT target inspectors, consumed by OITResolve.slang (not the per-surface switch in SurfaceShading).
// LAYER_COUNT additionally changes what the translucent BasePixelPass writes: exactly 1.0 per fragment
// into accum.a, turning the weight channel into a raw per-pixel fragment count (fp16-exact to 2048) --
// separates "the fragment SET strobes" from "the fragment VALUES strobe".
#define DEBUG_MODE_OIT_ACCUM_COLOR      18u
#define DEBUG_MODE_OIT_ACCUM_WEIGHT     19u
#define DEBUG_MODE_OIT_REVEALAGE        20u
#define DEBUG_MODE_OIT_LAYER_COUNT      21u
// Reflection-probe inspectors. INFLUENCE answers "does a probe reach this pixel, and which one";
// RADIANCE answers "what did that probe actually capture". The two failure modes are distinct and
// look identical in a lit view: a probe that isn't reaching the pixel and a probe that captured the
// wrong thing both just look like the sky.
#define DEBUG_MODE_PROBE_INFLUENCE      22u
#define DEBUG_MODE_PROBE_RADIANCE       23u

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
    FSSAOSettings     SSAOSettings;
    FCullData         CullData;
    FParallaxSettings ParallaxSettings;
};

// Octahedral 16-16 unit-normal decode (Slang lacks unpackSnorm2x16). Branchless wrap follows
// Cigolle et al, "A Survey of Efficient Representations for Independent Unit Vectors" (2014).
float3 UnpackNormal(uint Packed)
{
    // BFE folds to a single sign-extending bitfield op per channel rather
    // than relying on the compiler to recognize the shift-pair idiom.
    int2 i = int2(bitfieldExtract(int(Packed),  0, 16),
                  bitfieldExtract(int(Packed), 16, 16));
    float2 e = float2(i) * (1.0 / 32767.0);

    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));

    float t = max(-n.z, 0.0);
    n.xy -= float2(n.x >= 0.0 ? t : -t, n.y >= 0.0 ? t : -t);

    return normalize(n);
}

// Octahedral 15-15 tangent direction + 1-bit handedness sign. Matches
// Lumina::PackTangent. Returns (tangent.xyz, sign) where sign is +1 or -1
// and the bitangent is reconstructed as cross(N, tangent) * sign.
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

// Inverse of UnpackNormal: 16-16 signed octahedral. Standard oct encode (the decode above
// is the matching closed form), so re-packing a decoded normal round-trips.
uint PackNormal(float3 n)
{
    n = normalize(n);
    float2 e = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    if (n.z < 0.0)
    {
        e = (1.0 - abs(float2(e.y, e.x))) * float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    }
    int2 i = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(i.x) & 0xFFFFu) | ((uint(i.y) & 0xFFFFu) << 16);
}

// Inverse of UnpackTangent: 15-15 signed octahedral + handedness sign at bit 30.
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

// Octahedral unit-normal <-> [0,1]^2 (for storage in unorm render targets, e.g. the DBuffer normal slot).
// Distinct from PackNormal/UnpackNormal (which pack into a single uint); these keep the two oct coords
// as floats so they survive alpha blending. Approximate under blending but adequate for decal normals.
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

/**
 * Inverse-transpose of the model matrix's upper-left 3x3, for transforming normals under non-uniform
 * scale. Cofactor form -- no general inverse, and it stays finite for a singular scale.
 *
 * The indexing is ROW-wise and that is load-bearing, not incidental: `M[i]` on a Slang matrix is row i
 * (verified by constant-folding `float4x4(r0,r1,r2,r3)[0]`, which yields r0), and `float3x3(a,b,c)` fills
 * ROWS likewise. So this reads the rows of A, and returns a matrix whose ROWS are the cross products
 * below. For a matrix B with columns b0,b1,b2, inverse(B) is exactly that arrangement -- and here
 * b_i = row_i(A), i.e. B = A^T. The result is therefore inverse(A^T) = (A^-1)^T, the normal matrix.
 *
 * Reading these as COLUMNS instead would make the result inverse(A), which is wrong and differs only
 * under non-uniform scale -- so do not "fix" the naming into columns.
 */
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

// Triangular-PDF dither (Gjol/Playdead "Banding in Games"). Two decorrelated
// uniform hashes per channel subtract to a triangular distribution: flat noise
// floor, +/-1 LSB amplitude at 8-bit -- enough to fully break banding.
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