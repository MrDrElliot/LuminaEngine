#pragma once

#include "Core/Math/Math.h"

#include "Containers/Array.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Color.h"
#include "Core/Threading/Thread.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/MeshData.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/RenderResource.h"
#include "Renderer/ViewVolume.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Shared/SharedConstants.h"

#define SCENE_MAX_BOUNDS UINT64_MAX

#define VERIFY_SSBO_ALIGNMENT(Type) \
static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned");

constexpr int NumCascades = NUM_CASCADES;
constexpr int ClusterGridSizeX = 16;
constexpr int ClusterGridSizeY = 9;
constexpr int ClusterGridSizeZ = 24;

constexpr int NumClusters = ClusterGridSizeX * ClusterGridSizeY * ClusterGridSizeZ;

constexpr int GCSMCascadeSizes[NumCascades]   = { 2048, 2048, 2048, 2048 };
constexpr int GCSMAtlasWidth                  = 4096;
constexpr int GCSMAtlasHeight                 = 4096;
constexpr int GCSMCascadeOriginX[NumCascades] = { 0,    2048, 0,    2048 };
constexpr int GCSMCascadeOriginY[NumCascades] = { 0,    0,    2048, 2048 };

constexpr int GShadowAtlasResolution    = 4096;

constexpr int GMaxCullViews             = MAX_CULL_VIEWS;

namespace Lumina
{
    class CMaterialInterface;
    struct FSourceVertex;
    class CMaterial;
    class CStaticMesh;
}

namespace Lumina
{

    template<typename T>
    using TRenderVector = TFixedVector<T, 100>;

    constexpr uint32 INVALID_MESH_RESOLVE_HANDLE = ~0u;

    constexpr uint32 MESH_RESOLVE_STATE_STALE   = ~0u;  // never resolved, or invalidated since
    constexpr uint32 MESH_RESOLVE_STATE_NO_MESH = 0u;   // settled: there is nothing to resolve

    constexpr uint32 MAX_MESHLETS_PER_SURFACE_LOD = (1u << MESHLET_DRAW_INDEX_BITS);

    // FGPUInstance::SurfaceDescIndex when the instance's LOD is fixed and no view may re-select it.
    constexpr uint32 kNoSurfaceDescIndex = NO_SURFACE_DESC_INDEX;

    // Mutually-exclusive debug viz; values must match DEBUG_MODE_* in Common.slang.
    enum class ERenderSceneDebugFlags : uint8
    {
        None                = 0,
        Unlit               = 1,
        Meshlets            = 2,
        WorldNormal         = 3,
        ShadingNormal       = 4,
        BaseColor           = 5,
        Roughness           = 6,
        Metallic            = 7,
        AmbientOcclusion    = 8,
        Emissive            = 9,
        UV                  = 10,
        LightComplexity     = 11,
        ClusterGrid         = 12,
        ShadowCascades      = 13,
        ShadowPenumbra      = 14,
        GTAO                = 15,
        MaterialID          = 16,
        TriangleID          = 17,
        OITAccumColor       = 18,
        OITMoments          = 19,
        OITTransmittance    = 20,
        OITLayerCount       = 21,
        ProbeInfluence      = 22,
        ProbeRadiance       = 23,
        Num                 = 24,
    };

    constexpr FStringView RenderFlagsAsString(ERenderSceneDebugFlags Flags)
    {
        switch (Flags)
        {
            case ERenderSceneDebugFlags::None:              return "Lit";
            case ERenderSceneDebugFlags::Unlit:             return "Unlit";
            case ERenderSceneDebugFlags::Meshlets:          return "Meshlets";
            case ERenderSceneDebugFlags::WorldNormal:       return "World Normal";
            case ERenderSceneDebugFlags::ShadingNormal:     return "Shading Normal";
            case ERenderSceneDebugFlags::BaseColor:         return "Base Color";
            case ERenderSceneDebugFlags::Roughness:         return "Roughness";
            case ERenderSceneDebugFlags::Metallic:          return "Metallic";
            case ERenderSceneDebugFlags::AmbientOcclusion:  return "Ambient Occlusion";
            case ERenderSceneDebugFlags::Emissive:          return "Emissive";
            case ERenderSceneDebugFlags::UV:                return "UV";
            case ERenderSceneDebugFlags::LightComplexity:   return "Light Complexity";
            case ERenderSceneDebugFlags::ClusterGrid:       return "Light Clusters";
            case ERenderSceneDebugFlags::ShadowCascades:    return "Shadow Cascades";
            case ERenderSceneDebugFlags::ShadowPenumbra:    return "Shadow Penumbra";
            case ERenderSceneDebugFlags::GTAO:              return "GTAO";
            case ERenderSceneDebugFlags::MaterialID:        return "Material ID";
            case ERenderSceneDebugFlags::TriangleID:        return "Triangle ID";
            case ERenderSceneDebugFlags::OITAccumColor:     return "OIT Accum Color";
            case ERenderSceneDebugFlags::OITMoments:        return "OIT Moments";
            case ERenderSceneDebugFlags::OITTransmittance:  return "OIT Transmittance";
            case ERenderSceneDebugFlags::OITLayerCount:     return "OIT Layer Count";
            case ERenderSceneDebugFlags::ProbeInfluence:    return "Reflection Probe Influence";
            case ERenderSceneDebugFlags::ProbeRadiance:     return "Reflection Probe Radiance";
            default:                                        return "Lit";
        }
    }

    enum class ELightType : uint8
    {
        Directional,
        Point,
        Spot,

        Num,
    };
    
    enum class EGPUSceneSettingFlags : uint16
    {
        None    = 0,
        Unlit   = BIT(0),
        Lit     = BIT(1),
    };
    
    ENUM_CLASS_FLAGS(EGPUSceneSettingFlags);
    
    enum class EInstanceFlags : uint32
    {
        None                    = 0,
        Billboard               = BIT(0),
        Skinned                 = BIT(1),
        CastShadow              = BIT(2),
        ReceiveShadow           = BIT(3),
        TwoSided                = BIT(4),  // Skip backface cone cull.
        IgnoreOcclusionCulling  = BIT(5),
        Translucent             = BIT(6),
        Masked                  = BIT(7),

        Active                  = BIT(8),   // 0 = free slot; the cull skips it without reading its payload
        HasGeometry             = BIT(9),   // the mesh's meshlet header is resident
    };

    ENUM_CLASS_FLAGS(EInstanceFlags);
    
    struct FCameraData
    {
        FVector4 Location          = {};
        FVector4 Up                = {};
        FVector4 Right             = {};
        FVector4 Forward           = {};
        FMatrix4 View              = {};
        FMatrix4 InverseView       = {};
        FMatrix4 Projection        = {};
        FMatrix4 InverseProjection = {};
    };

    constexpr uint32 LIGHT_TYPE_MASK      = 0x0000FFFF; // lower 16 bits
    constexpr uint32 LIGHT_SHADOW_MASK    = 0xFFFF0000; // upper 16 bits
    constexpr int    LIGHT_SHADOW_SHIFT   = 16;

    // Mirror of ELightFlags in Common.slang -- keep values in lockstep.
    enum class ELightFlags : uint32
    {
        None        = 0,
        Directional = BIT(0),
        Point       = BIT(1),
        Spot        = BIT(2),
        CastShadow  = BIT(3),
        Volumetric  = BIT(4),
    };

    ENUM_CLASS_FLAGS(ELightFlags);

    struct FSceneImage
    {
        RHI::FTextureH      Texture;
        uint32              SampledSlot = RHI::kInvalidHeapSlot;
        TVector<uint32>     MipUAVSlots;
        RHI::FTextureDesc   Desc;

        bool                bOwned = false;

        bool IsValid() const { return RHI::IsValid(Texture); }
        explicit operator bool() const { return IsValid(); }

        int32  GetResourceID() const { return SampledSlot == RHI::kInvalidHeapSlot ? -1 : (int32)SampledSlot; }
        int32  GetMipUAVIndex(uint32 Mip) const { return Mip < (uint32)MipUAVSlots.size() ? (int32)MipUAVSlots[Mip] : -1; }
        FUIntVector2 GetExtent() const { return FUIntVector2(Desc.Dimension.x, Desc.Dimension.y); }
        uint32 GetSizeX() const { return Desc.Dimension.x; }
        uint32 GetSizeY() const { return Desc.Dimension.y; }
        uint32 GetNumMips() const { return Desc.MipCount; }
    };

    inline FSceneImage CreateSceneImage(const RHI::FTextureDesc& Desc, bool bSampled = true, bool bMipUAVs = false,
                                        uint32 ReuseSampledSlot = RHI::kInvalidHeapSlot)
    {
        FSceneImage Out;
        Out.Desc    = Desc;
        Out.Texture = RHI::CreateTexture(Desc);
        Out.bOwned  = true;
        if (bSampled)
        {
            if (ReuseSampledSlot != RHI::kInvalidHeapSlot)
            {
                RHI::HeapRepointTexture(RHI::Core::GetGlobalHeap(), ReuseSampledSlot, Out.Texture);
                Out.SampledSlot = ReuseSampledSlot;
            }
            else
            {
                Out.SampledSlot = RHI::HeapWriteTexture(RHI::Core::GetGlobalHeap(), Out.Texture);
            }
        }
        if (bMipUAVs)
        {
            Out.MipUAVSlots.resize(Desc.MipCount);
            for (uint32 Mip = 0; Mip < Desc.MipCount; ++Mip)
            {
                Out.MipUAVSlots[Mip] = RHI::HeapWriteRWTexture(RHI::Core::GetGlobalHeap(), Out.Texture, Mip);
            }
        }
        return Out;
    }

    NODISCARD inline FSceneImage BorrowSceneImage(const FSceneImage& Owner)
    {
        FSceneImage Copy = Owner;
        Copy.bOwned = false;
        return Copy;
    }

    NODISCARD inline uint32 DetachSampledSlot(FSceneImage& Image)
    {
        const uint32 Slot = Image.SampledSlot;
        Image.SampledSlot = RHI::kInvalidHeapSlot;
        return Slot;
    }

    // Immediate release: caller guarantees no in-flight GPU use (WaitIdle or frame-deferred externally).
    inline void ReleaseSceneImage(FSceneImage& Image)
    {
        if (!Image.IsValid())
        {
            Image = {};
            return;
        }
        if (Image.SampledSlot != RHI::kInvalidHeapSlot)
        {
            RHI::HeapFreeTexture(RHI::Core::GetGlobalHeap(), Image.SampledSlot);
        }
        for (uint32 Slot : Image.MipUAVSlots)
        {
            if (Slot != RHI::kInvalidHeapSlot)
            {
                RHI::HeapFreeRWTexture(RHI::Core::GetGlobalHeap(), Slot);
            }
        }
        RHI::FreeH(Image.Texture);
        Image = {};
    }

    // Frame-deferred counterpart to ReleaseSceneImage: hands the texture and its heap slots to the RHI's
    // single retirement queue, which destroys them once the GPU has finished with the slot they were
    // retired in. Clears the source, so ownership transfers rather than being shared.
    inline void RetireSceneImage(FSceneImage& Image)
    {
        if (!Image.IsValid())
        {
            Image = {};
            return;
        }
        RHI::Core::RetireSampledSlot(Image.SampledSlot);
        for (uint32 Slot : Image.MipUAVSlots)
        {
            RHI::Core::RetireStorageSlot(Slot);
        }
        RHI::Core::Retire(Image.Texture);
        Image = {};
    }

    // Plain device-local allocation reached only by GPU address (BDA).
    struct FSceneBuffer
    {
        RHI::GPUPtr Ptr  = 0;
        uint64      Size = 0;

        RHI::GPUPtr GetAddress() const { return Ptr; }
        uint64      GetSize() const { return Size; }
        explicit operator bool() const { return Ptr != 0; }
    };

    inline FSceneBuffer CreateSceneBuffer(uint64 Size)
    {
        const RHI::GPUPtr Ptr = RHI::Malloc(Size, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
        return FSceneBuffer{ Ptr, Ptr != 0 ? Size : 0 };
    }

    struct FShadowAtlasConfig
    {
        uint32 AtlasResolution    = GShadowAtlasResolution;    // Atlas is square: AtlasResolution x AtlasResolution.
        uint32 MaxTileResolution  = 1024;                      // Largest tile a single shadow can claim. Must be pow2.
        uint32 MinTileResolution  = 128;                       // Smallest leaf the quad-tree will subdivide to. Must be pow2.
    };

    struct FShadowTile
    {
        FVector2 UVOffset;     // Normalized origin (0-1 range) of this tile in the atlas.
        FVector2 UVScale;      // Normalized size (square: UVScale.x == UVScale.y).
    };

    // Quad-tree shadow atlas allocator. Tiles sized by projected radius; reset per-frame via FreeTiles().
    class FShadowAtlas
    {
    public:

        FShadowAtlas(const FShadowAtlasConfig& InConfig)
            : Config(InConfig)
        {
            MinLevel = Log2Floor(Config.MinTileResolution);
            MaxLevel = Log2Floor(Config.MaxTileResolution);
            NumLevels = (MaxLevel - MinLevel) + 1;
            FreeLists.resize(NumLevels);

            FreeTiles();
        }

        ~FShadowAtlas()
        {
            ReleaseSceneImage(ShadowAtlas);
        }

        // GPU image created lazily: the atlas is a scene member constructed before scene Init.
        void InitImage()
        {
            if (ShadowAtlas.IsValid())
            {
                return;
            }

            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(Config.AtlasResolution, Config.AtlasResolution, 1);
            Desc.Format    = EFormat::D32;
            Desc.Usage     = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;

            ShadowAtlas = CreateSceneImage(Desc);
        }

        // Quantizes up to next pow2 and clamps to [Min,Max]. Returns INDEX_NONE if full.
        int32 AllocateTile(uint32 DesiredPixels)
        {
            FScopeLock Lock(AllocMutex);

            const uint32 ClampedSize = Math::Clamp(RoundUpPow2(DesiredPixels), Config.MinTileResolution, Config.MaxTileResolution);
            const uint32 StartLevel  = Log2Floor(ClampedSize) - MinLevel;

            for (uint32 Level = StartLevel; Level < NumLevels; ++Level)
            {
                if (!FreeLists[Level].empty())
                {
                    FTileRect Rect = FreeLists[Level].back();
                    FreeLists[Level].pop_back();

                    // Split down to StartLevel; return last quadrant, push siblings back.
                    while (Level > StartLevel)
                    {
                        const uint32 Half = Rect.Size / 2;
                        const uint32 ChildLevel = Level - 1;
                        FreeLists[ChildLevel].push_back({ Rect.X + Half, Rect.Y,        Half });
                        FreeLists[ChildLevel].push_back({ Rect.X,        Rect.Y + Half, Half });
                        FreeLists[ChildLevel].push_back({ Rect.X + Half, Rect.Y + Half, Half });
                        Rect = { Rect.X, Rect.Y, Half };
                        --Level;
                    }

                    const int32 Handle = (int32)Tiles.size();
                    const float InvAtlas = 1.0f / (float)Config.AtlasResolution;
                    FShadowTile Tile;
                    Tile.UVOffset = FVector2(Rect.X * InvAtlas, Rect.Y * InvAtlas);
                    Tile.UVScale  = FVector2(Rect.Size * InvAtlas);
                    Tiles.push_back(Tile);
                    return Handle;
                }
            }
            return INDEX_NONE;
        }

        // Reseeds top-level free list with a grid of MaxTileResolution roots.
        void FreeTiles()
        {
            Tiles.clear();
            for (TVector<FTileRect>& Q : FreeLists)
            {
                Q.clear();   // keeps capacity -- avoids reallocating the free lists every frame
            }

            const uint32 RootSize = Config.MaxTileResolution;
            for (uint32 Y = 0; Y < Config.AtlasResolution; Y += RootSize)
            {
                for (uint32 X = 0; X < Config.AtlasResolution; X += RootSize)
                {
                    FreeLists[NumLevels - 1].push_back({ X, Y, RootSize });
                }
            }
        }

        const FShadowTile& GetTile(int32 TileIndex) const { return Tiles[TileIndex]; }
        const FSceneImage& GetImage() const { return ShadowAtlas; }

        const FShadowAtlasConfig& GetConfig() const { return Config; }
        const TVector<FShadowTile>& GetAllocatedTiles() const { return Tiles; }

    private:

        struct FTileRect
        {
            uint32 X;
            uint32 Y;
            uint32 Size;
        };

        static constexpr uint32 Log2Floor(uint32 V)
        {
            uint32 R = 0;
            while (V >>= 1) { ++R; }
            return R;
        }

        static constexpr uint32 RoundUpPow2(uint32 V)
        {
            if (V <= 1)
            {
                return 1;
            }
            --V;
            V |= V >> 1;  V |= V >> 2;  V |= V >> 4;
            V |= V >> 8;  V |= V >> 16;
            return V + 1;
        }

        FSceneImage ShadowAtlas;
        FShadowAtlasConfig Config;
        TVector<FShadowTile> Tiles;
        TVector<TVector<FTileRect>> FreeLists;   // Indexed by (log2(size) - MinLevel). Used LIFO; cleared (keeps capacity) per frame.
        FMutex AllocMutex;
        uint32 MinLevel  = 0;
        uint32 MaxLevel  = 0;
        uint32 NumLevels = 0;
    };
    
    struct FLightShadow
    {
        FVector2   AtlasUVOffset;
        FVector2   AtlasUVScale;

        int32       ShadowMapIndex;
        int32       LightIndex;
        int32       ShadowDataIndex;    // Index into FSceneLightData::Shadows[]
        int32       _Padding;           // std430 16-byte alignment.
    };

    VERIFY_SSBO_ALIGNMENT(FLightShadow)
    
    // Hot per-light data. Keeping it at 64 bytes cuts the L2 footprint of the inner loop ~10x.
    struct FLight
    {
        FVector3        Position;
        uint32          Color;

        FVector3        Direction;   // to-light: FROM surface TOWARD the light (sun & spot)
        float           Radius;

        float           Intensity;
        float           Falloff;
        FVector2        Angles;

        ELightFlags     Flags;
        int32           ShadowDataIndex;    // INDEX_NONE if no shadow

        float           VolumetricIntensity;
        float           VolumetricScatteringRadius;   // soft-core source radius (fraction of Radius) for spot/point fog
    };

    static_assert(sizeof(FLight) == 64, "FLight hot struct must fit a cache line");
    static_assert(eastl::is_trivially_copyable_v<FLight>);

    VERIFY_SSBO_ALIGNMENT(FLight)

    // Cold shadow-caster data; hot lighting loop never touches it.
    struct FLightShadowData
    {
        FMatrix4        ViewProjection[6];  // 384 B
        FLightShadow    Shadow[6];          // 192 B
    };

    static_assert(sizeof(FLightShadowData) == 576, "FLightShadowData layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FLightShadowData)

    struct FSkyLight
    {
        FVector4 Color;
    };

    struct FSceneLightData
    {
        uint32              NumLights{};
        // 1 when the environment IBL cubes are valid; 0 means skylight-only -> shader adds a flat ambient.
        uint32              bHasIBL{};
        uint32              Padding0[2];

        FVector3           SunDirection{};   // to-light: FROM surface TOWARD the sun (== Lights[0].Direction)
        uint32              bHasSun{};

        FVector4           CascadeSplits{};
        // Half-extent of each CSM cascade; used to convert shadow texel to world length.
        FVector4           CascadeRadii{};
        // Per-cascade shadow-map resolution; xyzw = cascades 0..3.
        FVector4           CascadeResolutions{};

        FVector4           ShadowParams{ 1.0f, 0.0f, 0.05f, 0.20f };
        // x = far-cascade distance-fade fraction; yzw reserved.
        FVector4           ShadowParams2{ 0.15f, 0.0f, 0.0f, 0.0f };

        FVector4           AmbientLight{};

        FLight              Lights[MAX_LIGHTS]{};
        FLightShadowData    Shadows[MAX_SHADOWS]{};
    };
    
    struct FLineBatch
    {
        uint32  StartVertex;
        uint32  VertexCount;
        float   Thickness;
        bool    bDepthTest;
    };

    struct FSolidBatch
    {
        uint32          StartVertex;
        uint32          VertexCount;
        ESolidDrawMode  Mode;
    };

    struct FGTAOSettings
    {
        float  Radius    = 0.5f;
        float  Intensity = 1.0f;
        float  Power     = 2.0f;
        float  Bias      = 0.025f;   // reserved (unused by GTAO)

        uint32 AOTextureIndex = ~0u;
        uint32 _Pad0 = 0;
        uint32 _Pad1 = 0;
        uint32 _Pad2 = 0;
    };

    // 32 byte layout, must match FBillboardInstance in Common.slang.
    struct alignas(16) FBillboardInstance
    {
        FVector3        Position;
        float           Size;

        uint32          ColorPack;
        uint32          TextureIndex;
        uint32          EntityID;
        uint32          _Pad0 = 0;
    };
    static_assert(sizeof(FBillboardInstance) == 32, "FBillboardInstance layout must match shader");

    // World-space UI widget quad. Matches FWidgetInstance in Common.slang (96B, dense).
    struct alignas(16) FWidgetInstance
    {
        FMatrix4        Transform;      // entity world matrix
        FVector2        WorldSize;      // quad size in world units
        uint32          TextureIndex;   // bindless ResourceID of the widget RT
        uint32          Flags;          // bit0 = billboard (face camera)
        uint32          ColorPack;      // tint, PackColor()
        uint32          EntityID;
        uint32          Pad0;
        uint32          Pad1;
    };

    static constexpr uint32 WIDGET_FLAG_BILLBOARD = 1u << 0;

    struct alignas(16) FGPUGlyph
    {
        FVector3 Origin;   float Pad0;   // world anchor (entity origin)
        FVector3 Right;    float Pad1;   // world right axis * worldEmSize
        FVector3 Up;       float Pad2;   // world up axis * worldEmSize
        FVector4 UVRect;                 // u0, v0, u1, v1
        FVector2 PlaneMin;               // quad min in the text plane
        FVector2 PlaneMax;               // quad max in the text plane
        uint32   ColorPack;              // PackColor()
        uint32   EntityID;               // for the picker pass
        uint32   Pad4;
        uint32   Pad5;
    };

    static_assert(sizeof(FGPUGlyph) == 96, "FGPUGlyph layout must match TextCommon.slang");

    struct alignas(16) FGPUDecal
    {
        FMatrix4    WorldToDecal;       // world -> decal-local ([-0.5,0.5]^3 inside the box)
        FMatrix4    DecalToWorld;       // decal-local cube -> world (entity transform)
        float       FadeAngleCos;       // cos(max angle) of surface normal vs decal forward; below => fades out
        float       Opacity;            // master coverage multiplier
        uint32      MaterialIndex;      // slot into the material uniform buffer
        uint32      Flags;              // reserved
    };

    static_assert(sizeof(FGPUDecal) == 144, "FGPUDecal layout must match DecalCommon.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUDecal)

    struct alignas(16) FGPUReflectionProbe
    {
        FMatrix4 WorldToProbe;     // world -> unit probe space
        FMatrix4 ProbeToWorld;     // brings the parallax hit point back to world
        FVector4 CapturePosition;  // xyz = world-space capture origin (cube center); w unused
        // x = brightness, y = shape (0 box, 1 sphere), z = cube-array slice, w = blend fraction
        FVector4 Params;
    };

    static_assert(sizeof(FGPUReflectionProbe) == 160, "FGPUReflectionProbe layout must match ReflectionProbe.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUReflectionProbe)

    struct FReflectionProbeCapture
    {
        FVector3 Position  = FVector3(0.0f);   // world-space capture origin (entity origin + CaptureOffset)
        float    NearPlane = 0.1f;
        float    FarPlane  = 500.0f;
        uint32   FaceSize  = 128u;             // per-probe capture resolution tier
        bool     bAlwaysUpdate = false;
        bool     bClearToColor = false;
        FVector3 ClearColor    = FVector3(0.0f);
    };

    // One water body. The water pass draws a procedural grid in [-0.5,0.5] (XZ) transformed by WaterToWorld
    struct alignas(16) FGPUWater
    {
        FMatrix4 WaterToWorld;      // local plane -> world (Extent baked into XZ scale)
        FMatrix4 WorldToWater;      // inverse
        FVector4 ShallowColor;      // rgb shallow tint
        FVector4 DeepColor;         // rgb deep tint
        FVector4 FoamColor;         // rgb foam tint
        FVector4 WindAndWave;       // xy = wind dir, z = wind speed, w = wave amplitude
        FVector4 WaveParams;        // x = choppiness, y = wave scale, z = wave count, w = detail strength
        FVector4 RefractReflect;    // x = refraction, y = reflection, z = roughness, w = fresnel power
        FVector4 FoamAbsorb;        // x = shoreline foam width, y = crest foam amount, z = depth fade, w = absorption
        FVector4 SSRSpecOpacity;    // x = ssr max dist, y = ssr step count, z = specular intensity, w = opacity
        FVector4 DetailParams;      // x = detail tiling, y = detail scroll speed, z = foam tiling, w = unused
        uint32   DetailNormalIndex; // bindless 2D SRV, ~0u if none
        uint32   FoamTextureIndex;  // bindless 2D SRV, ~0u if none
        uint32   GridResolution;    // verts per side of the procedural grid
        uint32   Flags;             // reserved
    };

    static_assert(sizeof(FGPUWater) == 288, "FGPUWater layout must match Includes/Water.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUWater)

    struct alignas(16) FWaterUnderwaterParams
    {
        FVector4 PlaneNormalAndHeight;  // xyz = surface up-normal, w = surface world Y under the camera
        FVector4 FogColorDensity;       // rgb = fog color, w = density (per meter)
        FVector4 TintDistortion;        // rgb = view tint, w = screen distortion amount
        FVector4 DeepColor;             // rgb = deep/absorption color
    };

    static_assert(sizeof(FWaterUnderwaterParams) == 64, "FWaterUnderwaterParams layout must match Includes/Water.slang");

    struct alignas(16) FCluster
    {
        FVector4 MinPoint;
        FVector4 MaxPoint;
        uint32 LightIndices[LIGHTS_PER_CLUSTER];
        uint32 Count;
    };
    
    VERIFY_SSBO_ALIGNMENT(FCluster)
    
    struct FLightClusterPC
    {
        FMatrix4 InverseProjection;
        FVector2 zNearFar;
        FUIntVector2 ScreenSize;
        FUIntVector4 GridSize;
    };

    struct alignas(16) FSurfaceDescGPU
    {
        uint32  LODMeshletOffset[MAX_MESH_LODS];
        uint32  LODMeshletCount[MAX_MESH_LODS];
        float   LODScreenThresholdSq[MAX_MESH_LODS];
        uint32  NumLODs;
        uint32  _Pad;
    };
    static_assert(sizeof(FSurfaceDescGPU) == 80, "FSurfaceDescGPU layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FSurfaceDescGPU)

    // Mirror of FRenderBucket in Common.slang: one (view, draw) pair's slice of the three cull arenas.
    // The CPU writes only the capacity seeds; BuildDrawPrefix owns every other field.
    struct FRenderBucketGPU
    {
        uint32 DrawBase;
        uint32 DrawCapacity;
        uint32 DrawCursor;
        uint32 BlockBase;
        uint32 BlockCapacity;
        uint32 BlockCursor;
        uint32 SubDrawCount;   // read directly as the indirect draw count; keep last
    };
    static_assert(sizeof(FRenderBucketGPU) == 28, "FRenderBucketGPU layout must match FRenderBucket in Common.slang");
    static_assert(offsetof(FRenderBucketGPU, SubDrawCount) % 4 == 0,
                  "SubDrawCount is used as a countBufferOffset, which must be 4-byte aligned");

    struct FTransform3x4
    {
        FVector4   Row0;
        FVector4   Row1;
        FVector4   Row2;
    };
    static_assert(sizeof(FTransform3x4) == 48, "FTransform3x4 must match shader");
    VERIFY_SSBO_ALIGNMENT(FTransform3x4)

    // Historical name, kept for the skinning path that is written in terms of bones.
    using FBoneTransform = FTransform3x4;

    // Drop the redundant 4th row of an affine matrix. p' = M*p == dot(Row_r, p4).
    FORCEINLINE FTransform3x4 PackTransform3x4(const FMatrix4& M)
    {
        return {
            FVector4(M[0][0], M[1][0], M[2][0], M[3][0]),
            FVector4(M[0][1], M[1][1], M[2][1], M[3][1]),
            FVector4(M[0][2], M[1][2], M[2][2], M[3][2]),
        };
    }

    struct alignas(16) FInstanceCullEntry
    {
        FVector4    SphereBounds;       // world space; xyz = center, w = radius
        uint32      DrawIDAndFlags;     // PackDrawIDAndFlags; carries Active and HasGeometry
        uint32      SurfaceDescIndex;   // into the interned LOD tables
        float       MaxDrawDistance;    // 0 = never distance-culled
        int32       ForcedLODIndex;     // -1 = automatic (distance / radius)
    };
    static_assert(sizeof(FInstanceCullEntry) == 32, "FInstanceCullEntry layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FInstanceCullEntry)

    struct alignas(16) FInstanceStatic
    {
        uint64      MeshletHeaderAddress;
        uint32      CustomData;
        uint32      MaterialIndex;
        uint32      EntityID;
        uint32      BoneOffset;
        uint32      SkinnedVertexBase;
        uint32      ShadowSkinnedVertexBase;
    };
    static_assert(sizeof(FInstanceStatic) == 32, "FInstanceStatic layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FInstanceStatic)

    struct FGPUInstance
    {
        FGPUInstance() noexcept {}

        FTransform3x4   Transform;              // offset   0
        FVector4        SphereBounds;           //         48
        uint64          MeshletHeaderAddress;   //         64

        uint32          DrawIDAndFlags;         //         72
        uint32          SurfaceMeshletOffset;   //         76
        uint32          SurfaceMeshletCount;    //         80
        uint32          ShadowMeshletOffset;    //         84
        uint32          ShadowMeshletCount;     //         88
        uint32          CustomData;             //         92

        uint32          BoneOffset;             //         96
        uint32          MaterialIndex;          //        100
        uint32          EntityID;               //        104
        uint32          SkinnedVertexBase;      //        108
        uint32          ShadowSkinnedVertexBase;//        112
        uint32          SurfaceDescIndex;       //        116
        uint32          MeshletTotalCount;      //        120
    };

    static_assert(sizeof(FGPUInstance) == 128, "FGPUInstance layout must match shader");
    static_assert(alignof(FGPUInstance) == 8, "Scalar layout: alignment comes from MeshletHeaderAddress");

    struct FPreSkinnedVertex
    {
        float       Px;
        float       Py;
        float       Pz;
        int16       NormalX;
        int16       NormalY;
        int16       NormalZ;
        uint32      Tangent;    // PackTangent
        uint32      UV;         // TEXCOORD_0
        uint32      UV1;        // TEXCOORD_1
        uint32      Color;
    };
    static_assert(sizeof(FPreSkinnedVertex) == 36, "FPreSkinnedVertex must match shader");

    constexpr uint32 kNoPreSkinBase = 0xFFFFFFFFu;

    struct FSkinDescriptor
    {
        uint64      MeshletHeaderAddress;   // FMeshletHeader* (BDA)
        uint32      BoneOffset;             // global index into the bone-matrix buffer
        uint32      SkinnedVertexBase;
        uint32      MeshletIndex;           // index into Header.Meshlets (one descriptor per meshlet)
        uint32      Pad;
    };
    static_assert(sizeof(FSkinDescriptor) == 24, "FSkinDescriptor must match shader");

    constexpr uint32 PackDrawIDAndFlags(uint32 DrawID, EInstanceFlags Flags)
    {
        return (DrawID & 0xFFFFu) | (((uint32)Flags & 0xFFFFu) << 16);
    }

    // CPU FFrustum (rich, SoA tail) -> 96-byte GPU plane mirror, and back (rebuilds the SoA).
    FORCEINLINE FGPUFrustum AsGPU(const FFrustum& F)
    {
        FGPUFrustum G;
        for (int i = 0; i < FFrustum::NUM; ++i)
        {
            G.Planes[i] = F.Planes[i];
        }
        return G;
    }

    FORCEINLINE FFrustum FromGPU(const FGPUFrustum& G)
    {
        FFrustum F;
        for (int i = 0; i < FFrustum::NUM; ++i)
        {
            F.Planes[i] = G.Planes[i];
        }
        F.RebuildSoA();
        return F;
    }

    struct FCullData
    {
        // Uploaded raw to the GPU -- plane-only mirrors, NOT the rich CPU FFrustum (see Frustum.h).
    // The camera the CULL evaluates against. Normally a straight copy of the render camera; they are
    // separate fields so freezing the cull cannot move the picture with it. Cull code must read these and
    // never CameraView, or a frozen cull would still track the live camera for distance, LOD and the
    // micro-poly projection.
        FVector4 CullCameraPosition;
        FMatrix4 CullCameraView;
        FMatrix4 CullCameraProjection;

        FGPUFrustum Frustum;
        FGPUFrustum ShadowFrustum;

        FGPUFrustum CascadeFrustum[NumCascades];

        FMatrix4 CascadeHZBViewProjection[NumCascades];
        // xy = pyramid-UV offset of the cascade's tile, zw = its UV scale.
        FVector4 CascadeHZBTile[NumCascades];
        FVector4 CascadeHZBNdcScale[NumCascades];

        FMatrix4 CascadeHZBViewProjectionMid[NumCascades];
        FVector4 CascadeHZBNdcScaleMid[NumCascades];

        uint32 bFrustumCull;
        uint32 bOcclusionCull;
        uint32 InstanceNum;
        uint32 bHasDirectional;

        float PyramidWidth;
        float PyramidHeight;

        float  ShadowMaxDistance;
        uint32 bShadowOcclusionCull;

        uint32 MeshletDrawTag;
        uint32 DebugMode;
        uint32 MeshletDrawListCapacity;
        // Bindless ResourceID of the depth pyramid; HZB tap goes through uBindlessTex2D.
        uint32 DepthPyramidIndex;

        uint32 CascadePyramidIndex;
        uint32 bCascadeHZBValid;
        uint32 bCascadeHZBMidValid;
        float  CascadePyramidWidth;
        float  CascadePyramidHeight;
        uint32 CascadePyramidMipCount;
        uint32 BoneNum;

        int32  ShadowLODBias;
        float  ShadowCoarseLODDistSq;
        uint32 _CullPad2;
        uint32 _CullPad3;
        uint32 _CullPad4;
    };

    VERIFY_SSBO_ALIGNMENT(FCullData)

    // Bits inside FCullView::Flags. Must match CULL_VIEW_FLAG_* in Common.slang.
    // Must match CULL_VIEW_FLAG_* in Common.slang.
    namespace ECullViewFlags
    {
        enum Type : uint32
        {
            None            = 0,
            Frustum         = BIT(0),
            Cone            = BIT(1),
            // Instance-level Hi-Z plus the meshlet-level sub-pixel reject; NOT meshlet occlusion.
            Occlusion       = BIT(2),
            Distance        = BIT(3),
            CastShadowOnly  = BIT(4),
            SunAligned      = BIT(5),
            // Meshlet-level Hi-Z. Only legal on a view that rasterizes BOTH VisBuffer phases -- a
            // single-phase view would defer meshlets that nothing ever re-tests, and they would not draw.
            MeshletHiZ      = BIT(6),
            Cascade         = BIT(7),
        };
    }

    // Which side of the mid-frame pyramid rebuild a meshlet cull dispatch is on.
    // Must match CULL_PHASE_* in Common.slang.
    namespace ECullPhase
    {
        enum Type : uint32
        {
            Early = 0,
            Late  = 1,
        };
    }

    // Mirror of FCullView in Common.slang; one entry per render view.
    struct alignas(16) FCullView
    {
        FVector4   FrustumPlanes[6];           // 96 B
        FVector4   ViewOriginAndFlags;         // 16 B: xyz=origin, w=asfloat(flags)
        uint32      CascadeIndex;               // Cascade this view rasterizes; only read when Flags has Cascade
        float       MinBoundsDiameter;          // Reject bounds thinner than this (world units); 0 = off
        uint32      IndirectArgsOffset;         // v * NumDraws
        uint32      NumDraws;                   // Number of indirect slots owned by this view
    };

    FORCEINLINE uint32 GetCullViewFlags(const FCullView& View)
    {
        uint32 Flags;
        std::memcpy(&Flags, &View.ViewOriginAndFlags.w, sizeof(uint32));
        return Flags;
    }

    static_assert(sizeof(FCullView) == 128, "FCullView layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FCullView)

    // Sim flag bitmask, must match constants in ParticleSimulate(.Template).slang
    static constexpr uint32 PARTICLE_SIM_FLAG_LOOP          = 1u << 0;
    static constexpr uint32 PARTICLE_SIM_FLAG_BURST_PENDING = 1u << 1;

    // 288 byte layout, must match FParticleSimParams in ParticleSimulate.slang / ParticleSimulateTemplate.slang.
    struct alignas(16) FParticleSimParamsGPU
    {
        FVector4  EmitterPosition;
        FVector4  EmitterForward;
        FVector4  EmitterRight;
        FVector4  EmitterUp;
        FUIntVector4 Counts;              // x=MaxParticles, y=SpawnCount, z=FrameSeed, w=SimFlags
        FUIntVector4 Modes;               // x=Shape, y=VelocityMode
        FVector4  ShapeSize;           // xyz dims; w=cone half-angle (radians)
        FVector4  VelocityMin;
        FVector4  VelocityMax;
        FVector4  SpeedAndLifetime;    // x=speedMin, y=speedMax, z=lifeMin, w=lifeMax
        FVector4  Gravity;             // xyz=gravity, w=drag
        FVector4  StartColor;
        FVector4  EndColor;
        FVector4  SizeRange;           // xy=start(min,max); zw=end(min,max)
        FVector4  RotationRange;       // xy=rot(min,max); zw=rotSpeed(min,max)
        FVector4  NoiseStrength;       // xyz=strength; w=scale
        FVector4  NoiseParams;         // x=speed
        FVector4  Timing;              // x=DeltaTime, y=TotalTime, z=SystemAge
    };
    static_assert(sizeof(FParticleSimParamsGPU) == 288, "FParticleSimParamsGPU layout must match shader");

    // 48 byte layout. must match FParticleRenderParams in ParticleVertex.slang.
    struct alignas(16) FParticleRenderParamsGPU
    {
        FUIntVector4 Flags;       // x=TextureIndex, y=BillboardToCamera
        FVector4  Tint;        // xyz=color, w=intensity
        FVector4  UVParams;    // reserved
    };
    static_assert(sizeof(FParticleRenderParamsGPU) == 48, "FParticleRenderParamsGPU layout must match shader");
    
    struct FGPUSceneSettings
    {
        EGPUSceneSettingFlags Flags;
    };

    struct FSceneRoot
    {
        uint64 SceneData             = 0;  // FSceneGlobalData (per-view camera/scene)
        uint64 Lights                = 0;
        uint64 Instances             = 0;
        uint64 Bones                 = 0;
        uint64 Clusters              = 0;  // per-view, GPU-written
        uint64 Materials             = 0;  // non-dynamic
        uint64 Billboards            = 0;
        uint64 CullViews             = 0;
        uint64 MeshletDrawList       = 0;  // ring, GPU-written
        uint64 PreSkinnedVertices    = 0;  // GPU-written
        uint64 SkinDescriptors       = 0;
        uint64 Widgets               = 0;
        uint64 ReflectionProbes      = 0;  // FGPUReflectionProbe array, sorted by descending priority

        uint32 BRDFLutIndex          = 0;
        uint32 SkyIrradianceIndex    = 0;
        uint32 SkyPrefilterIndex     = 0;
        uint32 ShadowCascadeIndex    = 0;  // bindless 2D SRV (cascade atlas)
        uint32 ShadowAtlasIndex      = 0;  // bindless 2D SRV (spot/point atlas)
        uint32 SkyCubeIndex          = 0;  // bindless cube SRV (full-res sky; sharp near-mirror reflections)
        uint32 ProbeCubeArrayIndex   = 0;
        uint32 NumReflectionProbes   = 0;
    };
    static_assert(sizeof(FSceneRoot) == 136, "FSceneRoot must match SceneGlobals.slang");

    struct FRootConstants
    {
        uint64 RootAddr = 0;
        uint64 PassAddr = 0;
    };

    struct FParallaxSettings
    {
        float SampleScale       = 1.0f;   // scales Min/Max sample counts; <= 0 disables POM outright
        float LODBias           = 0.0f;   // added to the LOD threshold; negative fades POM out nearer
        float ShadowSampleScale = 1.0f;   // scales self-shadow samples; 0 disables self-shadowing
        float _Pad0             = 0.0f;
    };

    struct FSceneGlobalData
    {
        FCameraData       CameraData;
        FUIntVector4      ScreenSize;
        FUIntVector4      GridSize;

        float           Time;
        float           DeltaTime;
        float           NearPlane;
        float           FarPlane;

        FGTAOSettings   GTAOSettings;
        FCullData       CullData;
        FParallaxSettings ParallaxSettings;

        uint32          ShadowMaskIndex   = ~0u;
        uint32          MomentZerothIndex = ~0u;
        uint32          MomentsIndex      = ~0u;
        uint32          _ShadowPad2       = 0;
    };

    struct FMeshPass
    {
        uint32 MeshDrawOffset;
        uint32 MeshDrawSize;
        uint32 IndirectDrawOffset;
    };
    
    // CPU-side scene stats. Draw-time counters come from FPipelineStats / FGPUProfileFrame.
    struct FSceneRenderStats
    {
        uint64 NumBatches = 0;
        uint64 NumMeshes = 0;
        uint64 NumMaterials = 0;
        uint64 NumDrawCallsCulled = 0;
        uint64 NumInstancesCulled = 0;
        uint64 NumShadowDraws = 0;
        uint64 NumSkinnedMeshes = 0;
        uint64 NumStaticMeshes = 0;
    };
    
    struct FSceneRenderSettings
    {
        ERenderSceneDebugFlags Flags    = ERenderSceneDebugFlags::None;
        uint8 bUseInstancing:1          = true;
        uint8 bHasEnvironment:1         = false;
        uint8 bDrawAABB:1               = false;
        uint8 bGTAO:1                   = false;
        uint8 bFrustumCull:1            = true;
        uint8 bConeCull:1               = true;
        uint8 bOcclusionCull:1          = true;
        // Per-MESHLET Hi-Z, resolved across the two VisBuffer phases. Separate from bOcclusionCull so the
        // instance-level cull (which is single-phase and costs nothing extra) can stay on while this one
        // is A/B'd -- turning it off collapses the frame back to a single geometry phase.
        uint8 bMeshletOcclusionCull:1   = true;
        uint8 bShadowOcclusionCull:1    = true;
        uint8 bWireframe:1              = false;
        uint8 bDrawBillboards:1         = true;
        uint8 bCPUInstanceCull:1        = true;
        uint8 bUseLODs:1                = true;
        uint8 bShadowMaskValid:1        = false;
        // Debug: keep culling against the inputs captured when this went on, so the selected set holds
        // still while the camera flies free. See FForwardRenderScene::ApplyCullFreeze.
        uint8 bFreezeCulling:1          = false;
        int8  ShadowLODBias             = 1;
        float ShadowCoarseLODDistance   = 150.0f;
    };
    
}
