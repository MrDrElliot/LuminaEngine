#pragma once

#include "Core/Math/Math.h"

#include "Containers/Vector.h"
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
#include "World/Scene/RenderScene/EnvironmentRenderTypes.h"
#include "Shared/SharedConstants.h"

#define SCENE_MAX_BOUNDS UINT64_MAX

#define VERIFY_SSBO_ALIGNMENT(Type) \
static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned")

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

    // FGPUInstance::RetainedSlot for an instance the CPU feeds directly (skinned), which has no retained
    // slot to key a persistent two-phase visibility flag off. Always out of range, so it reads as
    // "not visible last frame" and the late phase draws it.
    constexpr uint32 kNoRetainedSlot = 0xFFFFFFFFu;

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
        Specular            = 24,
        ShadingModel        = 25,
        Clearcoat           = 26,
        ClearcoatRoughness  = 27,
        SelfShadow          = 28,
        WireframeOverlay    = 29,
        Velocity            = 30,
        Num                 = 31,
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
            case ERenderSceneDebugFlags::Specular:          return "Specular";
            case ERenderSceneDebugFlags::ShadingModel:      return "Shading Model";
            case ERenderSceneDebugFlags::Clearcoat:         return "Clearcoat";
            case ERenderSceneDebugFlags::ClearcoatRoughness:return "Clearcoat Roughness";
            case ERenderSceneDebugFlags::SelfShadow:        return "Self Shadow";
            case ERenderSceneDebugFlags::WireframeOverlay:  return "Wireframe Overlay";
            case ERenderSceneDebugFlags::Velocity:          return "Velocity";
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
            RHI::SetDebugName(ShadowAtlas.Texture, "Scene.ShadowAtlas");
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

    VERIFY_SSBO_ALIGNMENT(FLightShadow);
    
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

    struct FSkyLight
    {
        FVector4 Color;
    };

    // Header only; the arrays hang off it by address so a frame uploads the live prefix, not the cap.
    struct FSceneLightData
    {
        uint32              NumLights{};
        // 1 when the environment IBL cubes are valid; 0 means skylight-only -> shader adds a flat ambient.
        uint32              bHasIBL{};
        // Bounds the Shadows allocation; every assigned ShadowDataIndex is below it.
        uint32              NumShadows{};
        uint32              Padding0{};

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

        uint64              LightsAddress{};    // FLight[NumLights]
        uint64              ShadowsAddress{};   // FLightShadowData[NumShadows]
    };

    static_assert(sizeof(FSceneLightData) == 144, "FSceneLightData layout must match FLightData in Common.slang");
    VERIFY_SSBO_ALIGNMENT(FSceneLightData);
    // Relaxed block layout rejects a vector straddling 16, so the pointers must follow the last one.
    static_assert(offsetof(FSceneLightData, LightsAddress) == 128, "LightsAddress must sit at 128");
    
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
        uint32   Flags;             // reserved
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

    struct alignas(16) FCluster
    {
        FVector4 MinPoint;
        FVector4 MaxPoint;
        uint32 LightIndices[LIGHTS_PER_CLUSTER];
        uint32 Count;
    };
    
    VERIFY_SSBO_ALIGNMENT(FCluster);
    
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
        // Per-slice (EMeshletSlice) view of the draw region, relative to DrawBase.
        uint32 SliceBase[kMeshletSliceCount];
        uint32 SliceCount[kMeshletSliceCount];
        // Read as the indirect draw count at offsetof(SubDrawCount) + slice * 4; keep last.
        uint32 SubDrawCount[kMeshletSliceCount];
    };
    static_assert(sizeof(FRenderBucketGPU) == 64, "FRenderBucketGPU layout must match FRenderBucket in Common.slang");
    static_assert(offsetof(FRenderBucketGPU, SubDrawCount) % 4 == 0,
                  "SubDrawCount is used as a countBufferOffset, which must be 4-byte aligned");

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
        int32       ForcedLODIndex;     // -1 = automatic (distance / radius)
    };
    static_assert(sizeof(FInstanceCullEntry) == 32, "FInstanceCullEntry layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FInstanceCullEntry);

    struct alignas(16) FInstanceStatic
    {
        uint32      MeshletHeaderSlot;      // into the process-wide slab; 0 = the null header
        uint32      CustomData;
        uint32      MaterialIndex;
        uint32      EntityID;
        uint32      BoneOffset;
        uint32      SkinnedVertexBase;
        uint32      ShadowSkinnedVertexBase;
        // Explicit, not tail padding: alignas(16) would supply it here but Slang's scalar layout would
        // size the mirror at 28 and stride the array wrong.
        uint32      _Pad0;
    };
    static_assert(sizeof(FInstanceStatic) == 32, "FInstanceStatic layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FInstanceStatic);

    struct FGPUInstance
    {
        FGPUInstance() noexcept {}

        FTransform3x4   Transform;              // offset   0
        FVector4        SphereBounds;           //         48
        uint32          MeshletHeaderSlot;      //         64  into the slab; 0 = the null header
        uint32          _Pad0;                  //         68  was the upper half of the header pointer

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
        uint32          RetainedSlot;           //        124 (was tail padding)
    };

    static_assert(sizeof(FGPUInstance) == 128, "FGPUInstance layout must match shader");
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

    // Mirror of the shader FSkinnedMeshletCone, one per skinned meshlet per frame.
    struct FSkinnedMeshletCone
    {
        FVector3 Apex;
        float    Cutoff;
        FVector3 Axis;
        float    ApexSpread;
    };
    static_assert(sizeof(FSkinnedMeshletCone) == 32, "FSkinnedMeshletCone must match shader");

    constexpr uint32 kNoPreSkinBase = 0xFFFFFFFFu;
    // No per-frame skinned meshlet bounds, so the cull falls back to bind-pose spheres and distrusts them.
    constexpr uint32 kNoSkinnedBounds = 0xFFFFFFFFu;

    // No slice in the per-frame bone arena; the blend falls back to identity rather than reading garbage.
    constexpr uint32 kNoBoneSlice = 0xFFFFFFFFu;

    /** Per-frame data for one skinned instance slot*/
    struct FSkinnedFrameData
    {
        // CullData.MeshletDrawTag as of the frame that wrote this. Anything else means the entity was not
        // gathered this frame, so the slot is stale and must not be emitted -- the array is never cleared.
        uint32      FrameTag;
        uint32      SurfaceMeshletOffset;
        uint32      SurfaceMeshletCount;
        uint32      ShadowMeshletOffset;
        uint32      ShadowMeshletCount;
        uint32      MeshletTotalCount;
        uint32      SkinnedVertexBase;          // kNoPreSkinBase = over budget, skin inline instead
        uint32      ShadowSkinnedVertexBase;
        // Slice in the COMPACTED per-frame bone arena; kNoBoneSlice when the gather assigned none.
        uint32      BoneOffset;
        // Folds in the range start, so it indexes the bounds arena by a mesh-global meshlet index.
        uint32      SkinnedBoundsBase;
    };
    // Unpadded on purpose; 48 straddles a 64-byte line as often as 40, on a retained-slot-sized array.
    static_assert(sizeof(FSkinnedFrameData) == 40, "FSkinnedFrameData must match shader");

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

        int32  ShadowLODBias;
        float  ShadowCoarseLODDistSq;
        uint32 _CullPad2;
        uint32 _CullPad3;
        uint32 _CullPad4;
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
    VERIFY_SSBO_ALIGNMENT(FCullView);

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
        uint64 Bones                 = 0;
        // Last frame's pose and transform, copied before this frame's incremental uploads overwrite them.
        uint64 PrevBones             = 0;
        uint64 PrevRetainedTransforms = 0;
        uint64 Clusters              = 0;  // per-view, GPU-written
        uint64 Materials             = 0;  // non-dynamic
        uint64 Billboards            = 0;
        uint64 CullViews             = 0;
        uint64 MeshletDrawList       = 0;  // ring, GPU-written
        uint64 PreSkinnedVertices    = 0;  // GPU-written
        uint64 Widgets               = 0;
        uint64 ReflectionProbes      = 0;  // FGPUReflectionProbe array, sorted by descending priority
        uint64 Splines               = 0;  // FGPUSpline headers, one per component with bSendToGPU
        uint64 SplinePoints          = 0;  // shared FGPUSplinePoint array; headers carry the slice
        uint64 SplineSamples         = 0;  // shared FGPUSplineSample array (arc-length tables)
        // The process-wide meshlet header slab. Instances carry a SLOT into this, never an address, so
        // this is the only place a header address exists -- and it is republished every frame, which is
        // what lets the slab grow without invalidating anything. See MeshletHeaderSlab.h.
        uint64 MeshletHeaders        = 0;

        /** Texture-streaming feedback: one uint per bindless texture slot, bit N set = "some pixel this
         *  frame sampled mip N of this texture". Written by the material lanes through
         *  RequestTextureResolution, read back a few frames later to drive residency.
         *
         *  This replaces guessing the requirement on the CPU from bounds, distance and texel density --
         *  the GPU already computes the exact LOD it samples, so ask it. 0 disables the write. */
        uint64 StreamingFeedback     = 0;
        // Object-space meshlet spheres for this frame's poses, written by SkinnedMeshletBounds.slang.
        uint64 SkinnedMeshletBounds  = 0;
        // Indexed by retained slot, and the cull reads only SkinnedBoundsBase out of it.
        uint64 SkinnedFrameData      = 0;
        // Same index space as SkinnedMeshletBounds, so one base addresses both.
        uint64 SkinnedMeshletCones   = 0;

        uint32 BRDFLutIndex          = 0;
        uint32 SkyIrradianceIndex    = 0;
        uint32 SkyPrefilterIndex     = 0;
        uint32 ShadowCascadeIndex    = 0;  // bindless 2D SRV (cascade atlas)
        uint32 ShadowAtlasIndex      = 0;  // bindless 2D SRV (spot/point atlas)
        uint32 SkyCubeIndex          = 0;  // bindless cube SRV (full-res sky; sharp near-mirror reflections)
        uint32 ProbeCubeArrayIndex   = 0;
        uint32 NumReflectionProbes   = 0;
        uint32 NumSplines            = 0;
        /** Entries in StreamingFeedback. Shaders bounds-check against this before indexing it or the
         *  bindless heap -- an unvalidated ResourceID is a device loss, not an artifact. */
        uint32 StreamingFeedbackCount = 0;
        // Entries in the snapshots above, which lag a frame and can be shorter than the live arrays.
        uint32 PrevBoneCount          = 0;
        uint32 PrevRetainedTransformCount = 0;
    };
    // 19 pointers + 12 indices; RHI::FSceneBindings is the only home for SceneData, Lights and Instances.
    static_assert(sizeof(FSceneRoot) == 200, "FSceneRoot must match SceneGlobals.slang");

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
        // Rounds the scalar run back to a 16-byte boundary, which the buffer layout rules require.
        float           _TimePad0 = 0.0f;
        float           _TimePad1 = 0.0f;
        float           _TimePad2 = 0.0f;

        FGTAOSettings   GTAOSettings;
        FCullData       CullData;
        FParallaxSettings ParallaxSettings;

        uint32          ShadowMaskIndex   = ~0u;
        uint32          MomentZerothIndex = ~0u;
        uint32          MomentsIndex      = ~0u;
        // Subsample index 0 or 1 that decorrelates screen-space noise. Stays 0 unless T2x is resolving.
        uint32          TemporalPhase     = 0;

        // The translucent passes fog themselves, since the composite runs first and sees only opaque depth.
        FExponentialHeightFogParams FogParams = {};

        uint32          FogIntegratedIndex   = ~0u;
        uint32          FogGridZ             = 0;
        float           FogNearPlane         = 0.05f;
        float           FogRange             = 200.0f;

        uint32          bFogEnabled          = 0;
        uint32          FogFarShaftSteps     = 0;
        float           FogFarShaftDistance  = 4000.0f;
        uint32          FogCloudShadowIndex  = ~0u;

        FVector2        FogCloudShadowCenter = FVector2(0.0f, 0.0f);
        float           FogCloudShadowExtent = 0.0f;
        float           _FogPad0             = 0.0f;
    };
    // alignas(16) here but 4-byte aligned in scalar layout, so they agree only with no C++ padding.
    static_assert(offsetof(FSceneGlobalData, FogParams) % 16 == 0,
                  "FogParams must land on 16 in FSceneGlobalData or C++ pads where the shader does not.");

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
        // still while the camera flies free. See FDefaultSceneRenderer::ApplyCullFreeze.
        uint8 bFreezeCulling:1          = false;
        int8  ShadowLODBias             = 1;
        float ShadowCoarseLODDistance   = 150.0f;
    };
    
}
