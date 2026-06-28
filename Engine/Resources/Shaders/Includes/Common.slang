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

// CullMeshlets phase: 0 walks all instance/meshlet pairs (emit or defer occluded);
// 1 walks only the defer list and re-tests against the freshly-built HZB.
#define CULL_PHASE_EARLY 0u
#define CULL_PHASE_LATE  1u

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
};

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
struct FMeshletHeader
{
    FMeshlet*       Meshlets;
    FMeshletBounds* Bounds;
    uint*           Vertices;
    uint*           Triangles;
};

// Mirror of Lumina::FSkinDescriptor. One per rendered-LOD meshlet; drives the skinning dispatch.
struct FSkinDescriptor
{
    FMeshletHeader* MeshletHeader;
    uint            BoneOffset;
    uint            SkinnedVertexBase;  // combined: compacted base - vertex span start
    uint            MeshletIndex;       // index into Header.Meshlets (one descriptor per meshlet)
    uint            Pad;
};

// Mirror of Lumina::FBoneTransform: a skinning matrix with the always-(0,0,0,1) last row
// dropped. Blends linearly like the matrix; transform a point by explicit row dot products.
struct FBoneTransform
{
    float4 Row0;
    float4 Row1;
    float4 Row2;
};

// One entry per surviving meshlet emitted by MeshletCull.slang.
struct FMeshletDraw
{
    uint InstanceID;         // Into Instances()
    uint MeshletLocalIndex;  // Local offset inside instance's surface range
};

// Camera meshlets the early pass deferred via Hi-Z; the late pass re-tests
// against the rebuilt HZB. DrawID is captured at defer time because the
// late pass increments uIndirectArgs[...DrawID].InstanceCount.
struct FMeshletDeferred
{
    uint InstanceID;
    uint MeshletLocalIndex;
    uint DrawID;
    uint _Pad;
};

// Mirror of Lumina::FGPUInstance.
struct FGPUInstance
{
    float4x4                ModelMatrix;
    float4                  SphereBounds;

    // Shadow LOD's meshlet range. CullMeshlets uses these for shadow-only
    // views; ShadowMappingVert reads ShadowMeshletOffset to locate the
    // emitted meshlet. Camera-side passes keep using SurfaceMeshlet*.
    uint                    ShadowMeshletOffset;
    uint                    ShadowMeshletCount;
    FMeshletHeader*         MeshletHeader;

    private uint            DrawIDAndFlags;
    uint                    SurfaceMeshletOffset;
    uint                    SurfaceMeshletCount;
    FCustomPrimitiveData    CustomData;

    // Full 32-bit index into the global bone-matrix buffer (no longer packed with the
    // material index), so the scene is not capped at 64k total bones.
    uint                    BoneOffset;
    uint                    MaterialIndex;
    uint                    EntityID;
    // Base into PreSkinnedVertices() for this instance's entity (skinning compute writes,
    // draw VS reads). 0 for static instances.
    uint                    SkinnedVertexBase;

    bool HasFlag(EInstanceFlags Flag)
    {
        uint Mask = (uint)Flag << 24;
        return (DrawIDAndFlags & Mask) == Mask;
    }

    property uint DrawID
    {
        get { return DrawIDAndFlags & 0x00FFFFFFu; }
        set { DrawIDAndFlags = (DrawIDAndFlags & 0xFF000000u) | (newValue & 0x00FFFFFFu); }
    }

    property EInstanceFlags Flags
    {
        get { return EInstanceFlags((DrawIDAndFlags >> 24) & 0xFFu); }
        set { DrawIDAndFlags = (DrawIDAndFlags & 0x00FFFFFFu) | ((newValue & 0xFFu) << 24); }
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
// IndirectArgs via DrawListOffset / IndirectArgsOffset.
struct FCullView
{
    float4 FrustumPlanes[6];   // 96B
    float4 ViewOriginAndFlags; // 16B: xyz=origin (camera / light pos), w=asuint(flags)
    uint   DrawListOffset;     // Into MeshletDrawList()
    uint   DrawListCapacity;   // Max FMeshletDraw entries this view may emit
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
    FFrustum CascadeFrustum[NUM_CASCADES];
    uint     bFrustumCull;
    uint     bOcclusionCull;
    uint     InstanceNum;
    uint     bHasDirectional;
    float    PyramidWidth;
    float    PyramidHeight;
    float    ShadowMaxDistance;
    uint     bShadowOcclusionCull;
    uint     NumDraws;
    uint     DebugMode;
    uint     TotalMeshletBound;
    uint     DepthPyramidIndex;
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

struct FSceneGlobalData
{
    FCameraView   CameraView;
    uint4         ScreenSize;
    uint4         GridSize;
    float         Time;
    float         DeltaTime;
    float         NearPlane;
    float         FarPlane;
    FSSAOSettings SSAOSettings;
    FCullData     CullData;
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

float3x3 CalculateNormalMatrix(float4x4 ModelMatrix)
{
    float3 C0 = ModelMatrix[0].xyz;
    float3 C1 = ModelMatrix[1].xyz;
    float3 C2 = ModelMatrix[2].xyz;

    float3 R0 = cross(C1, C2);
    float3 R1 = cross(C2, C0);
    float3 R2 = cross(C0, C1);

    float InvDet = 1.0 / dot(C0, R0);

    return float3x3(R0 * InvDet, R1 * InvDet, R2 * InvDet);
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

float3 ReconstructWorldPosition(float2 UV, float Depth, float4x4 InvProjection, float4x4 InvView)
{
    float  Z            = Depth * 2.0 - 1.0;
    float4 ClipSpacePos = float4(UV * 2.0 - 1.0, Z, 1.0);
    float4 ViewSpacePos = mul(InvProjection, ClipSpacePos);
    ViewSpacePos       /= ViewSpacePos.w;
    float4 WorldPos     = mul(InvView, ViewSpacePos);
    return WorldPos.xyz;
}

float3 ReconstructViewPosition(float2 UV, float Depth, float4x4 InvProjection)
{
    float4 ClipSpace = float4(UV * 2.0 - 1.0, Depth * 2.0 - 1.0, 1.0);
    float4 ViewSpace = mul(InvProjection, ClipSpace);
    return ViewSpace.xyz / ViewSpace.w;
}

float LinearizeDepth(float Depth, float Near, float Far)
{
    return (Near * Far) / (Far - Depth * (Far - Near)) / Far;
}

float LinearizeDepthReverseZ(float Depth, float Near, float Far)
{
    return Near * Far / (Far + Depth * (Near - Far));
}