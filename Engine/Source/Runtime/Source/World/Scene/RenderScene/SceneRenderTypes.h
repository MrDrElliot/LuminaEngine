#pragma once

#include "Core/Math/Math.h"
#include "Core/Math/Half.h"

#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Color.h"
#include "Core/Threading/Thread.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/MeshData.h"
#include "Renderer/GPUSpan.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/RenderResource.h"
#include "Renderer/ViewVolume.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "World/Scene/RenderScene/EnvironmentRenderTypes.h"
#include "Shared/SharedConstants.h"
#include "Lumina.h"

#define SCENE_MAX_BOUNDS UINT64_MAX

#define VERIFY_SSBO_ALIGNMENT(Type) \
static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned")

constexpr int NumCascades = NUM_CASCADES;
constexpr int ClusterGridSizeZ = 24;

// Screen pixels per cluster edge, fine enough that a cluster's list holds the lights its pixels can actually reach.
constexpr int ClusterTilePixels = 64;

// Every view's cluster buffer holds this many, which is 64 pixel tiles over 3840x2160; bigger views widen their tiles.
constexpr int MaxClusters = 64 * 32 * ClusterGridSizeZ;
constexpr int MaxClusterCullBlocks = 512;

static_assert(ClusterGridSizeZ % CLUSTER_CULL_BLOCK_Z == 0, "Z slices must fill whole cull blocks");

constexpr int GCSMCascadeSizes[NumCascades]   = { 2048, 2048, 2048, 2048 };
constexpr int GCSMAtlasWidth                  = 4096;
constexpr int GCSMAtlasHeight                 = 4096;
constexpr int GCSMCascadeOriginX[NumCascades] = { 0,    2048, 0,    2048 };
constexpr int GCSMCascadeOriginY[NumCascades] = { 0,    0,    2048, 2048 };

constexpr int GShadowAtlasResolution    = 4096;

constexpr int GMaxCullViews             = MAX_CULL_VIEWS;

namespace Lumina
{
    // Cluster counts per axis for a view, with the tile widened until the grid fits the buffer and the cull dispatch.
    inline FUIntVector4 ComputeClusterGrid(FUIntVector2 ScreenSize)
    {
        const uint32 Width  = Math::Max(ScreenSize.x, 1u);
        const uint32 Height = Math::Max(ScreenSize.y, 1u);
        constexpr uint32 TileStep = 16;
        for (uint32 Tile = ClusterTilePixels; ; Tile += TileStep)
        {
            const uint32 X = (Width + Tile - 1) / Tile;
            const uint32 Y = (Height + Tile - 1) / Tile;
            const uint32 Blocks = ((X + CLUSTER_CULL_BLOCK_X - 1) / CLUSTER_CULL_BLOCK_X)
                                * ((Y + CLUSTER_CULL_BLOCK_Y - 1) / CLUSTER_CULL_BLOCK_Y)
                                * (ClusterGridSizeZ / CLUSTER_CULL_BLOCK_Z);
            if (X * Y * ClusterGridSizeZ <= (uint32)MaxClusters && Blocks <= (uint32)MaxClusterCullBlocks)
            {
                return FUIntVector4(X, Y, ClusterGridSizeZ, 0);
            }
        }
    }

    class CMaterialInterface;
    struct FSourceVertex;
    class CMaterial;
    class CStaticMesh;
}

namespace Lumina
{

    template<typename T>
    using TRenderVector = TFixedVector<T, 100>;

    constexpr uint32 INVALID_MESH_RESOLVE_HANDLE = Constants::kIndexNoneU32;

    constexpr uint32 MESH_RESOLVE_STATE_STALE   = Constants::kIndexNoneU32;  // never resolved, or invalidated since
    constexpr uint32 MESH_RESOLVE_STATE_NO_MESH = 0u;   // settled: there is nothing to resolve

    constexpr uint32 MAX_MESHLETS_PER_SURFACE_LOD = (1u << MESHLET_DRAW_INDEX_BITS);

    // FGPUInstance::SurfaceDescIndex when the instance's LOD is fixed and no view may re-select it.
    constexpr uint32 kNoSurfaceDescIndex = NO_SURFACE_DESC_INDEX;

    // FGPUInstance::RetainedSlot for an instance the CPU feeds directly (skinned), which has no retained
    // slot to key a persistent two-phase visibility flag off. Always out of range, so it reads as
    // "not visible last frame" and the late phase draws it.
    constexpr uint32 kNoRetainedSlot = Constants::kIndexNoneU32;

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
        SunFarShadow        = 14,
        GTAO                = 15,
        MaterialID          = 16,
        TriangleID          = 17,
        OITAccumColor       = 18,
        OITTransmittance    = 20,
        OITLayerCount       = 21,
        ProbeInfluence      = 22,
        ProbeRadiance       = 23,
        Specular            = 24,
        ShadingModel        = 25,
        Clearcoat           = 26,
        ClearcoatRoughness  = 27,
        SelfShadow          = 28,
        WireframeOverlay    = 29,
        Velocity            = 30,
        QuadEfficiency      = 31,
        Num                 = 32,
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
            case ERenderSceneDebugFlags::SunFarShadow:      return "Sun Far Shadow";
            case ERenderSceneDebugFlags::GTAO:              return "GTAO";
            case ERenderSceneDebugFlags::MaterialID:        return "Material ID";
            case ERenderSceneDebugFlags::TriangleID:        return "Triangle ID";
            case ERenderSceneDebugFlags::OITAccumColor:     return "OIT Accum Color";
            case ERenderSceneDebugFlags::OITTransmittance:  return "OIT Transmittance";
            case ERenderSceneDebugFlags::OITLayerCount:     return "OIT Layer Count";
            case ERenderSceneDebugFlags::ProbeInfluence:    return "Reflection Probe Influence";
            case ERenderSceneDebugFlags::ProbeRadiance:     return "Reflection Probe Radiance";
            case ERenderSceneDebugFlags::Specular:          return "Specular";
            case ERenderSceneDebugFlags::ShadingModel:      return "Shading Model";
            case ERenderSceneDebugFlags::Clearcoat:         return "Clearcoat";
            case ERenderSceneDebugFlags::ClearcoatRoughness:return "Clearcoat Roughness";
            case ERenderSceneDebugFlags::SelfShadow:        return "Self Shadow";
            case ERenderSceneDebugFlags::WireframeOverlay:  return "Wireframe Overlay";
            case ERenderSceneDebugFlags::Velocity:          return "Velocity";
            case ERenderSceneDebugFlags::QuadEfficiency:    return "Quad Efficiency";
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

        ShadowOnly              = BIT(10),  // survives shadow views only; every camera view rejects it
        NoDecals                = BIT(11),  // decals pass over it, for things that move through them
        NoContactShadows        = BIT(12),  // thin geometry whose screen-space contact march only returns noise
        CastFarShadow           = BIT(13),  // also drawn into the sun's far cascade, which every other caster skips
        HasDistanceField        = BIT(14),  // the mesh carries a distance field the sun's far shadows can trace
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
        // Last frame's jittered view-projection, so reprojection lands on the pixel the history actually holds.
        FMatrix4 PrevViewProjection = {};
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
        // Screen-space contact trace on top of this light's shadow map; local lights only.
        ContactShadow = BIT(5),
        // Masked by a light-function material whose atlas tile sits at LIGHT_FUNCTION_SLOT_SHIFT.
        LightFunction = BIT(6),
        // A one-sided rectangle. Angles holds its half width and height, and VolumetricScatteringRadius its packed right axis.
        Area        = BIT(7),
        // Bits 8-15 and 24-31 are not free, see LIGHT_FUNCTION_SLOT_SHIFT and kLightMinRoughnessShift.
    };

    ENUM_CLASS_FLAGS(ELightFlags);

    // Quantized minimum roughness rides the top byte of FLight::Flags, which keeps FLight one cache line.
    constexpr uint32 kLightMinRoughnessShift = 24;

    inline ELightFlags PackLightMinRoughness(ELightFlags Flags, float MinRoughness)
    {
        const uint32 Quantized = (uint32)(Math::Clamp(MinRoughness, 0.0f, 1.0f) * 255.0f + 0.5f);
        return (ELightFlags)((uint32)Flags | (Quantized << kLightMinRoughnessShift));
    }

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

    inline FSceneImage CreateSceneImage(const RHI::FTextureDesc& Desc, bool bSampled = true, bool bMipUAVs = false)
    {
        FSceneImage Out;
        Out.Desc    = Desc;
        Out.Texture = RHI::CreateTexture(Desc);
        Out.bOwned  = true;
        if (bSampled)
        {
            Out.SampledSlot = RHI::HeapWriteTexture(RHI::GetGlobalHeap(), Out.Texture);
        }
        if (bMipUAVs)
        {
            Out.MipUAVSlots.resize(Desc.MipCount);
            for (uint32 Mip = 0; Mip < Desc.MipCount; ++Mip)
            {
                Out.MipUAVSlots[Mip] = RHI::HeapWriteRWTexture(RHI::GetGlobalHeap(), Out.Texture, Mip);
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

    // Hands the texture and its heap slots to the RHI's retirement queue and clears the source.
    inline void RetireSceneImage(FSceneImage& Image)
    {
        if (!Image.IsValid())
        {
            Image = {};
            return;
        }
        RHI::RetireSampledSlot(Image.SampledSlot);
        for (uint32 Slot : Image.MipUAVSlots)
        {
            RHI::RetireStorageSlot(Slot);
        }
        RHI::Retire(Image.Texture);
        Image = {};
    }

    // Reached only by device address, so without a name a GPU fault in one is an unattributed number.
    inline RHI::FGPUAllocation CreateSceneBuffer(uint64 Size, const char* DebugName = nullptr)
    {
        const RHI::FGPUAllocation Allocation = RHI::Malloc(Size, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
        if (Allocation.Gpu != 0 && DebugName != nullptr)
        {
            RHI::SetDebugName(Allocation.Gpu, DebugName);
        }
        return Allocation;
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
            RetireSceneImage(ShadowAtlas);
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
            RHI::SetDebugName(ShadowAtlas.Texture, "Scene.ShadowAtlas");
        }

        // Quantizes up to next pow2 and clamps to [Min,Max]. Returns Constants::kIndexNone if full.
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
            return Constants::kIndexNone;
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

    VERIFY_SSBO_ALIGNMENT(FLightShadow);
    
    // Hot per-light data. Keeping it at 64 bytes cuts the L2 footprint of the inner loop ~10x.
    struct FLight
    {
        // Position and Radius share the first 16 bytes, so the clustered loop culls on one load.
        FVector3        Position;
        float           Radius;

        FVector3        Direction;   // to-light: FROM surface TOWARD the light (sun & spot)
        uint32          Color;

        float           Intensity;
        float           Falloff;
        FVector2        Angles;

        ELightFlags     Flags;
        int32           ShadowDataIndex;    // Constants::kIndexNone if no shadow

        float           VolumetricIntensity;
        float           VolumetricScatteringRadius;   // soft-core source radius (fraction of Radius) for spot/point fog
    };

    static_assert(sizeof(FLight) == 64, "FLight hot struct must fit a cache line");
    static_assert(std::is_trivially_copyable_v<FLight>);

    VERIFY_SSBO_ALIGNMENT(FLight);

    // Cold shadow-caster data; hot lighting loop never touches it.
    struct FLightShadowData
    {
        FMatrix4        ViewProjection[6];  // 384 B
        FLightShadow    Shadow[6];          // 192 B
    };

    static_assert(sizeof(FLightShadowData) == 576, "FLightShadowData layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FLightShadowData);

    // The plane a light function is projected through, indexed by the light's atlas tile.
    struct FLightFunction
    {
        // Spot and directional only. Right.w is the projection scale, 1/tan of the cone or 1/extent in meters.
        FVector4        Right;
        FVector4        Up;
    };

    static_assert(sizeof(FLightFunction) == 32, "FLightFunction layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FLightFunction);

    struct FSkyLight
    {
        FVector4 Color;
    };

    // Header only; the arrays hang off it by address so a frame uploads the live prefix, not the cap.
    struct FSceneLightData
    {
        // 1 when the environment IBL cubes are valid; 0 means skylight-only -> shader adds a flat ambient.
        uint32              bHasIBL{};
        // The true sun for the sky and atmosphere, which differs from SunDirection once moonlight takes over at night.
        FVector3           SkySunDirection{};

        FVector3           SunDirection{};   // to-light for surface lighting and shadows (== Lights[0].Direction)
        uint32              bHasSun{};

        FVector4           CascadeSplits{};
        // Half-extent of each CSM cascade; used to convert shadow texel to world length.
        FVector4           CascadeRadii{};
        // Per-cascade shadow-map resolution; xyzw = cascades 0..3.
        FVector4           CascadeResolutions{};
        // Ortho depth range of each cascade, which turns a shadow-map NDC z delta into world units.
        FVector4           CascadeDepthRanges{};

        // x = normal-bias scale, y = constant depth bias, z = PCF radius in cascade-0 texels, w = cascade blend fraction.
        FVector4           ShadowParams{ 1.0f, 0.0f, 2.0f, 0.0f };
        // x = far-cascade distance-fade fraction, y = PCF tap count, z = active cascade count, w = 1 when the last cascade is the far one.
        FVector4           ShadowParams2{ 0.1f, 4.0f, (float)NumCascades, 0.0f };

        FVector4           AmbientLight{};

        RHI::TGPUSpan<FLight>           Lights;
        RHI::TGPUSpan<FLightShadowData> Shadows;
        RHI::TGPUSpan<FLightFunction>   LightFunctions;

        // Bindless SRV of the light-function atlas, read only by lights flagged LightFunction.
        uint32                          LightFunctionAtlas{};
        uint32                          _LightFunctionPad[3]{};
    };

    static_assert(sizeof(FSceneLightData) == 208, "FSceneLightData layout must match FLightData in Common.slang");
    static_assert(offsetof(FSceneLightData, SunDirection) == 16, "SunDirection must sit at 16, matching the scalar layout in Common.slang");
    VERIFY_SSBO_ALIGNMENT(FSceneLightData);
    // Relaxed block layout rejects a vector straddling 16, so the spans must follow the last one.
    static_assert(offsetof(FSceneLightData, Lights) == 144, "Lights must sit at 144");
    
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

    // Tuning reaches the GTAO passes through their own push constants, so only the result lands here.
    struct FGTAOSettings
    {
        uint32 AOTextureIndex = Constants::kIndexNoneU32;
        // Sun occlusion traced past the cascades, read pixel for pixel by the opaque lighting.
        uint32 SunShadowMaskIndex = Constants::kIndexNoneU32;
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
        FVector3 Origin;   float Intensity;   // world anchor (entity origin), HDR color scale
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

    // World-space sprite quad. Matches FGPUSprite in SpriteCommon.slang (96B).
    struct alignas(16) FGPUSprite
    {
        FVector3 Origin;   float Pad0;   // world anchor (entity origin)
        FVector3 Right;    float Pad1;   // world right axis, unit length
        FVector3 Up;       float Pad2;   // world up axis, unit length
        FVector4 UVRect;                 // u0, v0, u1, v1, already flipped
        FVector2 PlaneMin;               // quad min in world units, relative to Origin
        FVector2 PlaneMax;               // quad max in world units, relative to Origin
        uint32   ColorPack;              // PackColor()
        uint32   EntityID;               // for the picker pass
        uint32   Flags;
        float    AlphaCutThreshold;
    };

    static_assert(sizeof(FGPUSprite) == 96, "FGPUSprite layout must match SpriteCommon.slang");

    static constexpr uint32 SPRITE_FLAG_ALPHA_CUT = 1u << 0;

    struct alignas(16) FGPUDecal
    {
        FMatrix4    WorldToDecal;       // world -> decal-local ([-0.5,0.5]^3 inside the box)
        FMatrix4    DecalToWorld;       // decal-local cube -> world (entity transform)
        float       FadeAngleCos;       // cos(max angle) of surface normal vs decal forward; below => fades out
        float       Opacity;            // master coverage multiplier
        uint32      MaterialIndex;      // slot into the material uniform buffer
        float       EmissionEnergy;
        FVector4    UVTransform;        // xy scale and zw offset of the atlas cell shown
        FVector4    Modulate;           // rgb tint, a albedo mix
        FVector4    Fades;              // x distance fade begin (negative disables), y its length, z upper fade, w lower fade
    };

    static_assert(sizeof(FGPUDecal) == 192, "FGPUDecal layout must match DecalCommon.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUDecal);

    struct alignas(16) FGPUReflectionProbe
    {
        FMatrix4 WorldToProbe;     // world -> unit probe space
        FMatrix4 ProbeToWorld;     // brings the parallax hit point back to world
        FVector4 CapturePosition;  // xyz = world-space capture origin (cube center); w unused
        // x = brightness, y = shape (0 box, 1 sphere), z = cube-array slice, w = blend fraction
        FVector4 Params;
    };

    static_assert(sizeof(FGPUReflectionProbe) == 160, "FGPUReflectionProbe layout must match ReflectionProbe.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUReflectionProbe);

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
        float    HorizonScale;      // outer ring's distance as a multiple of the body's, 1 when it has no horizon skirt
    };

    static_assert(sizeof(FGPUWater) == 288, "FGPUWater layout must match Includes/Water.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUWater);

    struct alignas(16) FWaterUnderwaterParams
    {
        FVector4 PlaneNormalAndHeight;  // xyz = surface up-normal, w = surface world Y under the camera
        FVector4 FogColorDensity;       // rgb = fog color, w = density (per meter)
        FVector4 TintDistortion;        // rgb = view tint, w = screen distortion amount
        FVector4 DeepColor;             // rgb = deep/absorption color
    };

    static_assert(sizeof(FWaterUnderwaterParams) == 64, "FWaterUnderwaterParams layout must match Includes/Water.slang");

    // One spline control point, world space (the entity transform is baked in at extract).
    struct alignas(16) FGPUSplinePoint
    {
        FVector3 Location;      float Roll;      // Roll in degrees
        FVector3 ArriveTangent; float _Pad0;
        FVector3 LeaveTangent;  float _Pad1;
        FVector3 Scale;         float _Pad2;
    };

    static_assert(sizeof(FGPUSplinePoint) == 64, "FGPUSplinePoint layout must match Includes/Spline.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUSplinePoint);

    // One entry of a spline's arc-length table. Entries are uniform in DISTANCE, not in curve key, so a
    // shader converts a distance to an index with a single divide -- see SampleSplineAtDistance.
    struct alignas(16) FGPUSplineSample
    {
        FVector3 Position; float DistanceAlong;
        FVector3 Tangent;  float Key;           // curve key (0..NumSegments) this sample landed on
        FVector3 Up;       float Roll;          // degrees
        FVector3 Scale;    float _Pad;
    };

    static_assert(sizeof(FGPUSplineSample) == 64, "FGPUSplineSample layout must match Includes/Spline.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUSplineSample);

    static constexpr uint32 SPLINE_FLAG_CLOSED_LOOP = 1u << 0;

    // Header for one uploaded spline. Points and samples live in two shared arrays; this carries the slice.
    struct alignas(16) FGPUSpline
    {
        FMatrix4 LocalToWorld;      // entity transform the points were baked with
        FMatrix4 WorldToLocal;      // inverse, for anything projecting world positions back onto the curve
        uint32   PointOffset;       // first entry in the shared point array
        uint32   PointCount;
        uint32   SampleOffset;      // first entry in the shared sample array
        uint32   SampleCount;
        float    TotalLength;       // world-space arc length
        uint32   Flags;             // SPLINE_FLAG_*
        uint32   EntityID;          // owning entity, so a shader can correlate back
        uint32   _Pad;
    };

    static_assert(sizeof(FGPUSpline) == 160, "FGPUSpline layout must match Includes/Spline.slang");
    VERIFY_SSBO_ALIGNMENT(FGPUSpline);

    // Only LightCull reads the bounds, so shading's per-pixel word range lives in its own dense array.
    struct alignas(16) FCluster
    {
        FVector4 MinPoint;
        FVector4 MaxPoint;
    };

    VERIFY_SSBO_ALIGNMENT(FCluster);
    static_assert(sizeof(FCluster) == 32, "FCluster layout must match FCluster in Common.slang");
    
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
        float   LODError[MAX_MESH_LODS];
        uint32  NumLODs;
        uint32  _Pad;
    };
    static_assert(sizeof(FSurfaceDescGPU) == 80, "FSurfaceDescGPU layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FSurfaceDescGPU);

    // Mirror of FRenderBucket in Common.slang: one (view, draw) pair's slice of the three cull arenas.
    // The CPU writes only the capacity seeds; BuildDrawPrefix owns every other field.
    // Which part of a bucket's draw region a pass rasterizes. Mirrors MESHLET_SLICE_* in
    // Shared/SharedConstants.h; the meshlet cull writes all three and every draw picks one.
    enum class EMeshletSlice : uint32
    {
        Early = MESHLET_SLICE_EARLY,   // what the early cull phase appended
        Late  = MESHLET_SLICE_LATE,    // what the late phase added on top
        All   = MESHLET_SLICE_ALL,     // the whole region; final once both phases have run
    };
    constexpr uint32 kMeshletSliceCount = MESHLET_SLICE_COUNT;

    struct FRenderBucketGPU
    {
        uint32 DrawBase;
        uint32 DrawCapacity;
        uint32 DrawCursor;
        uint32 BlockBase;
        uint32 BlockCapacity;
        uint32 BlockCursor;
        // Dense offset of this bucket's blocks in the flat meshlet-cull dispatch. Blocks are dense within
        // a bucket but sparse across the arena, so the cull cannot derive its bucket from the block.
        uint32 CullWorkBase;
        uint32 DrawBudgetCursor;
        uint32 DrawDemand;
        uint32 BlockDemand;
        uint32 BlockBackCount;
        // Per-slice (EMeshletSlice) view of the draw region, relative to DrawBase.
        uint32 SliceBase[kMeshletSliceCount];
        uint32 SliceCount[kMeshletSliceCount];
        // Read as the indirect draw count at offsetof(SubDrawCount) + slice * 4; keep last.
        uint32 SubDrawCount[kMeshletSliceCount];
    };
    static_assert(sizeof(FRenderBucketGPU) == 80, "FRenderBucketGPU layout must match FRenderBucket in Common.slang");
    // Shaders reach this with loadAligned<16>, which needs every element 16-aligned.
    static_assert(sizeof(FRenderBucketGPU) % 16 == 0, "FRenderBucketGPU stride must stay 16-byte aligned for loadAligned<16>");

    static_assert(offsetof(FRenderBucketGPU, SubDrawCount) % 4 == 0,
                  "SubDrawCount is used as a countBufferOffset, which must be 4-byte aligned");

    // One view of a multi-view shadow draw, mirroring FShadowRasterView in MeshletGeometry.slang.
    struct FShadowRasterViewGPU
    {
        int32 ShadowDataIndex = -1;
        int32 ViewIndex       = 0;
        float TileSize[2]     = {};
    };
    static_assert(sizeof(FShadowRasterViewGPU) == 16, "FShadowRasterViewGPU must match FShadowRasterView in MeshletGeometry.slang.");

    struct FTransform3x4
    {
        FVector4   Row0;
        FVector4   Row1;
        FVector4   Row2;
    };
    static_assert(sizeof(FTransform3x4) == 48, "FTransform3x4 must match shader");
    VERIFY_SSBO_ALIGNMENT(FTransform3x4);

    // 32B, not 48: the 3x3 halves error-bound by the mesh extent, the translation kept full.
    struct FPackedBoneTransform
    {
        uint32 Rot[5];   // 9 halves of the 3x3, row-major; the 10th is unused
        float  Tx;
        float  Ty;
        float  Tz;
    };
    static_assert(sizeof(FPackedBoneTransform) == 32, "FPackedBoneTransform must match shader");
    // Shaders reach this with loadAligned<16>, which needs every element 16-aligned.
    static_assert(sizeof(FPackedBoneTransform) % 16 == 0, "FPackedBoneTransform stride must stay 16-byte aligned for loadAligned<16>");


    using FBoneTransform = FPackedBoneTransform;

    FORCEINLINE FPackedBoneTransform PackBoneTransform(const FMatrix4& M)
    {
        FPackedBoneTransform Out;
        Out.Rot[0] = Math::PackHalf2x16(FVector2(M[0][0], M[1][0]));
        Out.Rot[1] = Math::PackHalf2x16(FVector2(M[2][0], M[0][1]));
        Out.Rot[2] = Math::PackHalf2x16(FVector2(M[1][1], M[2][1]));
        Out.Rot[3] = Math::PackHalf2x16(FVector2(M[0][2], M[1][2]));
        Out.Rot[4] = Math::PackHalf2x16(FVector2(M[2][2], 0.0f));
        Out.Tx     = M[3][0];
        Out.Ty     = M[3][1];
        Out.Tz     = M[3][2];
        return Out;
    }

    FORCEINLINE FPackedBoneTransform IdentityBoneTransform()
    {
        return PackBoneTransform(FMatrix4(1.0f));
    }

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
        uint32      LODAndThinning;     // PackLODAndThinning
    };
    static_assert(sizeof(FInstanceCullEntry) == 32, "FInstanceCullEntry layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FInstanceCullEntry);

    // One instance cull workgroup, so a block the camera is too far from is skipped by the whole group at once.
    constexpr uint32 kInstanceCullBlockSize = 64;

    // Conservative distance bounds of kInstanceCullBlockSize consecutive retained slots.
    struct alignas(16) FInstanceBlockBounds
    {
        FVector3    Min;            // of every live slot's sphere
        float       Reach;          // largest MaxDrawDistance in the block; negative when nothing in it can draw
        FVector3    Max;
        uint32      bAlwaysScan;    // a slot with no draw distance, or slots the GPU writes that the CPU never sees
    };
    static_assert(sizeof(FInstanceBlockBounds) == 32, "FInstanceBlockBounds layout must match CullInstances.slang");
    VERIFY_SSBO_ALIGNMENT(FInstanceBlockBounds);

    struct alignas(16) FInstanceStatic
    {
        uint32      MeshletHeaderSlot;      // into the process-wide slab; 0 = the null header
        uint32      CustomData;
        uint32      MaterialIndex;
        uint32      EntityID;
        uint32      BoneOffset;
        uint32      SkinnedVertexBase;
        // Assigned by FScenePrimitiveSet::AssignMeshletVisibility; kNoMeshletVisibility for none.
        uint32      MeshletVisibilityBase;
        uint32      _Pad0;
    };
    // Word 0 is never handed out, so a zeroed payload reads as an instance with no meshlet visibility.
    constexpr uint32 kNoMeshletVisibility = 0u;
    static_assert(sizeof(FInstanceStatic) == 32, "FInstanceStatic layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FInstanceStatic);

    struct FGPUInstance
    {
        FGPUInstance() noexcept {}

        FTransform3x4   Transform;              // offset   0
        FVector4        SphereBounds;           //         48
        uint32          MeshletHeaderSlot;      //         64  into the slab; 0 = the null header
        uint32          MeshletVisibilityBase;  //         68

        uint32          DrawIDAndFlags;         //         72
        uint32          SurfaceMeshletOffset;   //         76
        uint32          SurfaceMeshletCount;    //         80
        uint32          CustomData;             //         84

        uint32          BoneOffset;             //         88
        uint32          MaterialIndex;          //         92
        uint32          EntityID;               //         96
        uint32          SkinnedVertexBase;      //        100
        uint32          SurfaceDescIndex;       //        104
        uint32          MeshletTotalCount;      //        108
        uint32          RetainedSlot;           //        112
        uint32          _Pad0[3];               //        116
    };

    static_assert(sizeof(FGPUInstance) == 128, "FGPUInstance layout must match shader");
    // Shaders reach this with loadAligned<16>, which needs every element 16-aligned.
    static_assert(sizeof(FGPUInstance) % 16 == 0, "FGPUInstance stride must stay 16-byte aligned for loadAligned<16>");

    // No 8-byte member left now that the header is a slot rather than a pointer, so scalar layout puts
    // this at 4. The 128-byte stride is unchanged, which is what the shader mirror actually depends on.
    static_assert(alignof(FGPUInstance) == 4, "Scalar layout: FGPUInstance is all 4-byte members");

    struct FPreSkinnedVertex
    {
        float       Px;
        float       Py;
        float       Pz;
        uint32      Normal;     // PackNormal
        uint32      Tangent;    // PackTangent
        uint32      UV;         // TEXCOORD_0
        uint32      UV1;        // TEXCOORD_1
        uint32      Color;
    };
    // 32 B lands every element on a sector boundary, so a warp's skinned writes fill whole sectors.
    static_assert(sizeof(FPreSkinnedVertex) == 32, "FPreSkinnedVertex must match shader");

    struct FPrevSkinnedPosition
    {
        float X;
        float Y;
        float Z;
    };
    static_assert(sizeof(FPrevSkinnedPosition) == 12, "FPrevSkinnedPosition must match Common.slang");
    // Shaders reach this with loadAligned<16>, which needs every element 16-aligned.
    static_assert(sizeof(FPreSkinnedVertex) % 16 == 0, "FPreSkinnedVertex stride must stay 16-byte aligned for loadAligned<16>");


    constexpr uint32 kNoPreSkinBase = Constants::kIndexNoneU32;
    // No per-frame skinned meshlet bounds, so the cull falls back to bind-pose spheres and distrusts them.
    constexpr uint32 kNoSkinnedBounds = Constants::kIndexNoneU32;

    // No slice in the per-frame bone arena; the blend falls back to identity rather than reading garbage.
    constexpr uint32 kNoBoneSlice = Constants::kIndexNoneU32;

    /** Per-frame data for one skinned instance slot*/
    struct FSkinnedFrameData
    {
        // CullData.MeshletDrawTag as of the frame that wrote this. Anything else means the entity was not
        // gathered this frame, so the slot is stale and must not be emitted -- the array is never cleared.
        uint32      FrameTag;
        uint32      SurfaceMeshletOffset;
        uint32      SurfaceMeshletCount;
        uint32      MeshletTotalCount;
        uint32      SkinnedVertexBase;          // kNoPreSkinBase = over budget, skin inline instead
        // Slice in the COMPACTED per-frame bone arena; kNoBoneSlice when the gather assigned none.
        uint32      BoneOffset;
        // Folds in the range start, so it indexes the bounds arena by a mesh-global meshlet index.
        uint32      SkinnedBoundsBase;
    };
    static_assert(sizeof(FSkinnedFrameData) == 28, "FSkinnedFrameData must match shader");

    // The forced LOD plus one in the low byte (0 is automatic) and the foliage thinning start distance as a half float in the high half.
    inline uint32 PackLODAndThinning(int32 ForcedLODIndex, float ThinningStartDistance)
    {
        const uint32 LOD = (uint32)Math::Clamp(ForcedLODIndex + 1, 0, 255);
        return LOD | ((uint32)FHalf(Math::Clamp(ThinningStartDistance, 0.0f, 65000.0f)).Bits << 16);
    }

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

        // The cull camera clip range, so the sphere projection and the Hi-Z tap agree with CullCameraProjection.
        float  CullNearPlane;
        float  CullFarPlane;
        // Holds the struct at a 16-byte multiple, which the members after it in FSceneGlobalData rely on.
        uint32 _CullPad0 = 0;
        uint32 _CullPad1 = 0;
        uint32 _CullPad2 = 0;
    };

    VERIFY_SSBO_ALIGNMENT(FCullData);

    // Bits inside FCullView::Flags. Must match CULL_VIEW_FLAG_* in Common.slang.
    // Must match CULL_VIEW_FLAG_* in Common.slang.
    namespace ECullViewFlags
    {
        enum Type : uint32
        {
            None            = 0,
            Frustum         = BIT(0),
            Cone            = BIT(1),
            Occlusion       = BIT(2),
            Distance        = BIT(3),
            CastShadowOnly  = BIT(4),
            SunAligned      = BIT(5),
            MeshletHiZ      = BIT(6),
            Cascade         = BIT(7),
            FarCascade      = BIT(8),
            OrthographicLOD = BIT(9),
        };
    }

    // Which side of the mid-frame pyramid rebuild a pass is on. CPU-side only: the shaders read the
    // EMeshletSlice this maps to, which is what actually reaches them.
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
        float       LODErrorScale;              // MakeLODErrorScale; 0 keeps every instance at LOD 0
        uint32      _Pad0;
    };

    FORCEINLINE uint32 GetCullViewFlags(const FCullView& View)
    {
        uint32 Flags;
        std::memcpy(&Flags, &View.ViewOriginAndFlags.w, sizeof(uint32));
        return Flags;
    }

    static_assert(sizeof(FCullView) == 128, "FCullView layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FCullView);

    // Sim flag bitmask, must match constants in ParticleSimulate(.Template).slang
    static constexpr uint32 PARTICLE_SIM_FLAG_LOOP          = 1u << 0;
    static constexpr uint32 PARTICLE_SIM_FLAG_BURST_PENDING = 1u << 1;
    static constexpr uint32 PARTICLE_SIM_FLAG_LOCAL_SPACE   = 1u << 2;

    // Sentinel for "consumes no event list", must match ParticleSimCommon.slang.
    static constexpr uint32 PARTICLE_NO_EVENT_LIST = Constants::kIndexNoneU32;

    // Above the EParticleBlendMode byte of the sprite pass RenderFlags, must match ParticleSpriteCommon.slang.
    static constexpr uint32 PARTICLE_RENDER_FLAG_LIT = 1u << 8;

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
        FVector4  Timing;              // x=DeltaTime, y=TotalTime, z=SystemAge, w=EmitterScale
        FMatrix4  EmitterDelta;        // last frame's emitter transform to this frame's, for local space
        FVector4  EventParams;         // x=continuous events per second raised, y=parent velocity inherited
        FUIntVector4 EventInfo;        // x=event lists raised, y=list consumed, z=emission mesh slot, w=particles per event
        FUIntVector4 RibbonInfo;       // x=segments, y=head sample, z=head advanced this step
    };
    static_assert(sizeof(FParticleSimParamsGPU) == 400, "FParticleSimParamsGPU layout must match shader");

    // Mirrors FParticleEvent in ParticleSimCommon.slang, and doubles as an EmitParticle call from script.
    struct alignas(16) FParticleEventGPU
    {
        FVector4  PositionSize;        // xyz position, w size
        FVector4  VelocityFlags;       // xyz velocity, w EParticleEmitFlags as float bits
        FVector4  Extra;               // a script emit's color, or a collision's surface normal in xyz and impact speed in w
    };
    static_assert(sizeof(FParticleEventGPU) == 48, "FParticleEventGPU layout must match shader");

    // An attractor or collider volume, rigid so its axes are unit length and Extent carries the scale.
    struct alignas(16) FParticleShapeGPU
    {
        FVector4  Center;              // xyz world center, w EParticleShapeType
        FVector4  AxisX;
        FVector4  AxisY;
        FVector4  AxisZ;
        FVector4  Extent;              // xyz half extents, x alone for a sphere's radius
        FVector4  Params;              // x strength, y attenuation, z directionality
    };
    static_assert(sizeof(FParticleShapeGPU) == 96, "FParticleShapeGPU layout must match shader");

    struct alignas(16) FParticleTerrainGPU
    {
        FVector4  OriginSize;          // x origin X, y origin Z, z tile world size, w base height
        FVector4  HeightParams;        // x max height
        FUIntVector4 Textures;         // x heightmap, y normal map
    };
    static_assert(sizeof(FParticleTerrainGPU) == 48, "FParticleTerrainGPU layout must match shader");

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
        RHI::TGPUSpan<FBoneTransform>       Bones;
        // Last frame's pose and transform, copied before this frame's incremental uploads overwrite them.
        // These lag a frame and can be shorter than the live arrays, which is what their counts say.
        RHI::TGPUSpan<FBoneTransform>       PrevBones;
        RHI::TGPUSpan<FTransform3x4>        PrevRetainedTransforms;
        RHI::TGPUSpan<FCluster>          Clusters;              // per-view, GPU-written
        // Per-view light bitmasks, ClusterMaskWords words per cluster.
        RHI::TGPUSpan<uint32>            ClusterLightMasks;
        // Per cluster, its first non-empty mask word in the low 16 bits and one past its last in the high 16.
        RHI::TGPUSpan<uint32>            ClusterWordRanges;
        FMaterialTableGPU                Materials;
        RHI::TGPUSpan<FMaterialCollectionUniforms> Collections;         // slot 0 is the reserved zero one
        RHI::TGPUSpan<FBillboardInstance> Billboards;
        RHI::TGPUSpan<FCullView>         CullViews;
        RHI::TGPUSpan<FUIntVector2>         MeshletDrawList;       // ring, GPU-written
        RHI::TGPUSpan<FPreSkinnedVertex>    PreSkinnedVertices;    // GPU-written
        RHI::TGPUSpan<FWidgetInstance>   Widgets;
        RHI::TGPUSpan<FGPUReflectionProbe>  ReflectionProbes;      // sorted by descending priority
        RHI::TGPUSpan<FGPUSpline>           Splines;               // one per component with bSendToGPU
        RHI::TGPUSpan<FGPUSplinePoint>      SplinePoints;          // headers carry the slice
        RHI::TGPUSpan<FGPUSplineSample>     SplineSamples;         // arc-length tables
        // The process-wide meshlet header slab. Instances carry a SLOT into this, never an address, so
        // this is the only place a header address exists -- and it is republished every frame, which is
        // what lets the slab grow without invalidating anything. See MeshletHeaderSlab.h.
        RHI::TGPUSpan<FMeshletHeaderGPU>    MeshletHeaders;

        /** Texture-streaming feedback: one uint per bindless texture slot, bit N set = "some pixel this
         *  frame sampled mip N of this texture". Written by the material lanes through
         *  RequestTextureResolution, read back a few frames later to drive residency.
         *
         *  This replaces guessing the requirement on the CPU from bounds, distance and texel density --
         *  the GPU already computes the exact LOD it samples, so ask it. An empty span disables the
         *  write, and its count is what keeps an unvalidated slot out of the bindless heap. */
        RHI::TGPUSpan<uint32>               StreamingFeedback;
        // Object-space meshlet spheres for this frame's poses, written by SkinnedMeshletBounds.slang.
        RHI::TGPUSpan<FMeshletSphere>       SkinnedMeshletBounds;
        // Indexed by retained slot, and the cull reads only SkinnedBoundsBase out of it.
        RHI::TGPUSpan<FSkinnedFrameData>    SkinnedFrameData;
        // Same index space as SkinnedMeshletBounds, so one base addresses both.
        RHI::TGPUSpan<FMeshletCone>         SkinnedMeshletCones;
        // Parallel to PreSkinnedVertices, and empty when no view wants motion vectors.
        RHI::TGPUSpan<FPrevSkinnedPosition> PreSkinnedPrevPositions;

        uint32 BRDFLutIndex          = 0;
        uint32 SkyIrradianceIndex    = 0;
        uint32 SkyPrefilterIndex     = 0;
        uint32 ShadowCascadeIndex    = 0;  // bindless 2D SRV (cascade atlas)
        uint32 ShadowAtlasIndex      = 0;  // bindless 2D SRV (spot/point atlas)
        uint32 SkyCubeIndex          = 0;  // bindless cube SRV (full-res sky; sharp near-mirror reflections)
        uint32 ProbeCubeArrayIndex   = 0;
        uint32 _Pad0                 = 0;
    };
    static_assert(sizeof(FSceneRoot) == 416, "FSceneRoot must match SceneGlobals.slang");

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
        float           PrevTime;
        float           DeltaTime;
        float           NearPlane;
        float           FarPlane;
        // This frame's projection jitter in UV units, so motion vectors measure from where a surface really is.
        float           TemporalJitterU = 0.0f;
        float           TemporalJitterV = 0.0f;
        // Added to every material texture's mip level, below zero while the view renders under the display resolution.
        float           TextureMipBias = 0.0f;

        FGTAOSettings   GTAOSettings;
        FCullData       CullData;
        FParallaxSettings ParallaxSettings;

        // Subsample index 0 or 1 that decorrelates screen-space noise. Stays 0 unless T2x is resolving.
        uint32          TemporalPhase     = 0;

        // Opaque scene depth, for the screen-space traces that run after the depth pass.
        uint32          SceneDepthIndex   = Constants::kIndexNoneU32;
        // This view's decal layers, ~0u when no decal rendered, so every opaque pass composites them.
        uint32          DBufferAIndex     = Constants::kIndexNoneU32;
        uint32          DBufferBIndex     = Constants::kIndexNoneU32;
        uint32          DBufferCIndex     = Constants::kIndexNoneU32;
        uint32          _PadFogParams[3]  = {};

        // The translucent passes fog themselves, since the composite runs first and sees only opaque depth.
        FExponentialHeightFogParams FogParams = {};

        uint32          FogIntegratedIndex   = Constants::kIndexNoneU32;
        uint32          FogGridZ             = 0;
        float           FogNearPlane         = 0.05f;
        float           FogRange             = 200.0f;

        uint32          bFogEnabled          = 0;
        uint32          FogFarShaftSteps     = 0;
        float           FogFarShaftDistance  = 4000.0f;
        uint32          FogCloudShadowIndex  = Constants::kIndexNoneU32;

        FVector2        FogCloudShadowCenter = FVector2(0.0f, 0.0f);
        float           FogCloudShadowExtent = 0.0f;
        // The additive emission layer decals write, ~0u when no decal rendered.
        uint32          DBufferDIndex        = Constants::kIndexNoneU32;
    };
    // alignas(16) here but 4-byte aligned in scalar layout, so C++ must not pad in front of it, which the 16 check alone cannot see.
    static_assert(offsetof(FSceneGlobalData, FogParams) % 16 == 0 &&
                  offsetof(FSceneGlobalData, FogParams) == offsetof(FSceneGlobalData, _PadFogParams) + sizeof(FSceneGlobalData::_PadFogParams),
                  "FogParams must follow the field before it with no C++ padding, since the shader's scalar layout has none.");

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
    
    // Recomputed by the renderer every extract, so it is deliberately not part of the world's settings.
    struct FSceneFrameFlags
    {
        uint8 bHasEnvironment:1  = false;
        uint8 bGTAO:1            = false;
    };

    // Owned by the world as a registry singleton, so it outlives a renderer that is destroyed and rebuilt.
    struct FSceneRenderSettings
    {
        ERenderSceneDebugFlags Flags    = ERenderSceneDebugFlags::None;
        uint8 bUseInstancing:1          = true;
        uint8 bDrawAABB:1               = false;
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
        // Rejects a local light whose attenuation sphere reaches no shading volume, before it takes a slot.
        uint8 bCullLights:1             = true;
        uint8 bUseLODs:1                = true;
        // Debug: keep culling against the inputs captured when this went on, so the selected set holds
        // still while the camera flies free. See FDefaultSceneRenderer::ApplyCullFreeze.
        uint8 bFreezeCulling:1          = false;
    };
    
}
