#pragma once

#include "Core/Math/Math.h"

#include "Containers/Array.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Color.h"
#include "Core/Threading/Thread.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/RenderResource.h"
#include "Renderer/ViewVolume.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"

#define MAX_LIGHTS 8192
#define MAX_SHADOWS 256
#define LIGHT_INDEX_MASK 0x1FFFu
#define LIGHTS_PER_UINT 2
#define LIGHTS_PER_CLUSTER 100

#define SCENE_MAX_BOUNDS UINT64_MAX

#define COL_R_SHIFT 0
#define COL_G_SHIFT 8
#define COL_B_SHIFT 16
#define COL_A_SHIFT 24
#define COL_A_MASK 0xFF000000

#define VERIFY_SSBO_ALIGNMENT(Type) \
static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned");

constexpr int NumCascades = 4;
constexpr int ClusterGridSizeX = 16;
constexpr int ClusterGridSizeY = 9;
constexpr int ClusterGridSizeZ = 24;

constexpr int NumClusters = ClusterGridSizeX * ClusterGridSizeY * ClusterGridSizeZ;

// 4 equal cascades in a 4096x4096 atlas (2x2). Equal resolution avoids starving the far cascades that
// cover the most ground; total D32 atlas is 67MB (vs the old 6144x4096 = 100MB with a 4096/2048/1024
// geometric falloff that spent most of the budget on the near cascade).
constexpr int GCSMCascadeSizes[NumCascades]   = { 2048, 2048, 2048, 2048 };
constexpr int GCSMAtlasWidth                  = 4096;
constexpr int GCSMAtlasHeight                 = 4096;
constexpr int GCSMCascadeOriginX[NumCascades] = { 0,    2048, 0,    2048 };
constexpr int GCSMCascadeOriginY[NumCascades] = { 0,    0,    2048, 2048 };

constexpr int GShadowAtlasResolution    = 4096;

// Hard cap on cull views: camera + NumCascades + 6/point + 1/spot.
constexpr int GMaxCullViews             = 128;

namespace Lumina
{
    class CMaterialInterface;
    struct FVertex;
    class CMaterial;
    class CStaticMesh;
}

namespace Lumina
{

    template<typename T>
    using TRenderVector = TFixedVector<T, 100>;
    
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
        SSAO                = 15,
        MaterialID          = 16,
        TriangleID          = 17,
        OITAccumColor       = 18,
        OITAccumWeight      = 19,
        OITRevealage        = 20,
        OITLayerCount       = 21,
        Num                 = 22,
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
            case ERenderSceneDebugFlags::SSAO:              return "SSAO";
            case ERenderSceneDebugFlags::MaterialID:        return "Material ID";
            case ERenderSceneDebugFlags::TriangleID:        return "Triangle ID";
            case ERenderSceneDebugFlags::OITAccumColor:     return "OIT Accum Color";
            case ERenderSceneDebugFlags::OITAccumWeight:    return "OIT Accum Weight";
            case ERenderSceneDebugFlags::OITRevealage:      return "OIT Revealage";
            case ERenderSceneDebugFlags::OITLayerCount:     return "OIT Layer Count";
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

    // Scene-owned new-RHI texture plus its global-heap slots. Value type; whoever created it
    // releases it (views may alias copies of shared images -- only the owner releases).
    struct FSceneImage
    {
        RHI::FTextureH      Texture;
        uint32              SampledSlot = RHI::kInvalidHeapSlot;
        TVector<uint32>     MipUAVSlots;
        RHI::FTextureDesc   Desc;

        // True only on the instance that created the texture (CreateSceneImage). Copies that alias
        // someone else's image leave it false, so a bulk "release everything I own" sweep can never
        // double-free a borrowed one. This is what makes ownership explicit on a freely-copied
        // handle type -- FSceneImage deliberately is NOT self-releasing, because the renderer copies
        // it around (view seeding, aliases, deferred-release queues) and a destructor-owns model
        // would need move-only semantics throughout.
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

    // bSampled registers an SRV heap slot; bMipUAVs registers one storage slot per mip.
    inline FSceneImage CreateSceneImage(const RHI::FTextureDesc& Desc, bool bSampled = true, bool bMipUAVs = false)
    {
        FSceneImage Out;
        Out.Desc    = Desc;
        Out.Texture = RHI::CreateTexture(Desc);
        Out.bOwned  = true;
        if (bSampled)
        {
            Out.SampledSlot = RHI::HeapWriteTexture(RHI::Core::GetGlobalHeap(), Out.Texture);
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

    // A non-owning copy of an image owned elsewhere. Use this whenever an image is handed to a second
    // holder (e.g. a view snapshotting the scene's shared IBL cubes): the copy reads identically but
    // is skipped by ownership-driven release, so only the real owner ever frees it.
    NODISCARD inline FSceneImage BorrowSceneImage(const FSceneImage& Owner)
    {
        FSceneImage Copy = Owner;
        Copy.bOwned = false;
        return Copy;
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
            RHI::HeapFreeRWTexture(RHI::Core::GetGlobalHeap(), Slot);
        }
        RHI::FreeH(Image.Texture);
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

    // Size is only recorded when the allocation actually landed. Reporting the requested size for a
    // null pointer makes every later ResizeBufferIfNeeded think the buffer is big enough, so nothing
    // retries and the GPU writes off a null base.
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

        // Directional shadow tuning (SDirectionalLightComponent): x = normal-bias scale, y = depth bias,
        // z = PCSS softness (light size), w = cascade cross-fade fraction.
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
        uint32  StartVertex;
        uint32  VertexCount;
        bool    bDepthTest;
    };

    // GTAO tuning; must match FSSAOSettings in Common.slang. No CPU kernel: the shader generates
    // its slice directions from per-pixel interleaved gradient noise.
    struct FSSAOSettings
    {
        float  Radius    = 0.5f;
        float  Intensity = 1.0f;
        float  Power     = 2.0f;
        float  Bias      = 0.025f;   // reserved (unused by GTAO)

        // Bindless SRV index of the per-view AO output the base pass samples. ~0u = no SSAO this
        // view (e.g. capture views, which never run the SSAO pass) -> base pass treats AO as 1.
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

    // One world-space text glyph quad. Origin/Right/Up define the text plane in world space (Right/Up
    // pre-scaled by the world em size); PlaneMin/Max are the quad bounds in that plane (em units) and
    // UVRect indexes the font's MSDF atlas. Must match FGPUGlyph in TextCommon.slang (scalar layout, 96B).
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

    // One projected decal. Drawn as a unit cube; the decal pixel shader reconstructs the surface from
    // depth, projects into decal-local space, and writes the DBuffer. Must match FGPUDecal in DecalCommon.slang.
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

    // The single active water body the camera is submerged in / near, consumed by the underwater
    // post-process pass. Must match FWaterUnderwaterParams in WaterUnderwater.slang.
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

    // 144B per-instance descriptor. Empty ctor skips zero-init on resize() (parallel writer overwrites everything).
    struct alignas(16) FGPUInstance
    {
        FGPUInstance() noexcept {}

        FMatrix4     Transform;
        FVector4       SphereBounds;

        uint32          ShadowMeshletOffset;
        uint32          ShadowMeshletCount;
        uint64          MeshletHeaderAddress;

        uint32          DrawIDAndFlags;
        uint32          SurfaceMeshletOffset;
        uint32          SurfaceMeshletCount;
        uint32          CustomData;

        // Full 32-bit bone index (was packed into 16 bits with MaterialIndex, which capped
        // the scene at 64k total bones).
        uint32          BoneOffset;
        uint32          MaterialIndex;
        uint32          EntityID;
        // Base index into the pre-skinned vertex buffer for this instance's surface-LOD meshlet block;
        // the skinning pass writes there, the draw VS reads instead of re-skinning. 0 for static
        // instances, kNoPreSkinBase when the entity lost the pre-skin budget.
        uint32          SkinnedVertexBase;
        // Same, for the shadow-LOD block. Equal to SkinnedVertexBase when the shadow LOD resolved to
        // the same block, which is the common case; the two differ only when the LODs diverge.
        uint32          ShadowSkinnedVertexBase;
        // Declared, not implied: under scalar layout the shader's array stride comes from the
        // declared members, so trailing padding has to exist on both sides or the strides diverge.
        uint32          _Pad[3];
    };

    static_assert(sizeof(FGPUInstance) == 144, "FGPUInstance layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FGPUInstance)

    // One per skinned vertex, produced by the skinning pass and read by every draw VS. Holds the COMPLETE
    // vertex so the VS never touches the source. Position full-precision; normal/tangent octahedral. 28 B.
    struct FPreSkinnedVertex
    {
        float       Px;
        float       Py;
        float       Pz;
        uint32      Normal;     // PackNormal
        uint32      Tangent;    // PackTangent
        uint32      UV;
        uint32      Color;
    };
    static_assert(sizeof(FPreSkinnedVertex) == 28, "FPreSkinnedVertex must match shader");

    // FGPUInstance::SkinnedVertexBase sentinel for "not pre-skinned this frame; blend in the draw
    // path". Mirrors kNoPreSkinBase in Common.slang. Reserved during merge so a wrapped base
    // (base = compacted base - span start) can never produce it by accident.
    constexpr uint32 kNoPreSkinBase = 0xFFFFFFFFu;

    // One per rendered-LOD meshlet; drives the skinning compute dispatch (one workgroup each).
    // Flattened from per-entity so every meshlet skins concurrently (no serial meshlet loop).
    struct FSkinDescriptor
    {
        uint64      MeshletHeaderAddress;   // FMeshletHeader* (BDA)
        uint32      BoneOffset;             // global index into the bone-matrix buffer
        // Combined base = (compacted slice base) - (vertex span start), so that
        // SkinnedVertexBase + M.VertexOffset lands in the compacted slice (uint wraps).
        uint32      SkinnedVertexBase;
        uint32      MeshletIndex;           // index into Header.Meshlets (one descriptor per meshlet)
        uint32      Pad;
    };
    static_assert(sizeof(FSkinDescriptor) == 24, "FSkinDescriptor must match shader");

    // Bone skinning matrix with its always-(0,0,0,1) last row dropped: first 3 rows of the 4x4.
    // 48 B vs 64 B, lossless for affine transforms. Read only by the skinning compute.
    struct FBoneTransform
    {
        FVector4   Row0;
        FVector4   Row1;
        FVector4   Row2;
    };
    static_assert(sizeof(FBoneTransform) == 48, "FBoneTransform must match shader");
    VERIFY_SSBO_ALIGNMENT(FBoneTransform)

    // Drop the redundant 4th row of an affine skinning matrix. p' = M*p == dot(Row_r, p4).
    FORCEINLINE FBoneTransform PackBoneTransform(const FMatrix4& M)
    {
        return {
            FVector4(M[0][0], M[1][0], M[2][0], M[3][0]),
            FVector4(M[0][1], M[1][1], M[2][1], M[3][1]),
            FVector4(M[0][2], M[1][2], M[2][2], M[3][2]),
        };
    }

    constexpr uint32 PackDrawIDAndFlags(uint32 DrawID, EInstanceFlags Flags)
    {
        return (DrawID & 0x00FFFFFF) | (((uint32)Flags & 0xFF) << 24);
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
        FGPUFrustum Frustum;
        FGPUFrustum ShadowFrustum;

        FGPUFrustum CascadeFrustum[NumCascades];

        uint32 bFrustumCull;
        uint32 bOcclusionCull;
        uint32 InstanceNum;
        uint32 bHasDirectional;

        float PyramidWidth;
        float PyramidHeight;

        float  ShadowMaxDistance;
        uint32 bShadowOcclusionCull;

        uint32 NumDraws;
        uint32 DebugMode;
        // Total meshlets this frame; flat thread-per-meshlet dispatch reads this.
        uint32 TotalMeshletBound;
        // Bindless ResourceID of the depth pyramid; HZB tap goes through uBindlessTex2D.
        uint32 DepthPyramidIndex;
    };

    // Bits inside FCullView::Flags. Must match CULL_VIEW_FLAG_* in Common.slang.
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
            PhaseLate       = BIT(6),
        };
    }

    // Phase push-constant values. Must match CULL_PHASE_* in Common.slang.
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
        uint32      DrawListOffset;             // Into uMeshletDrawList
        uint32      DrawListCapacity;           // Max FMeshletDraw entries this view may emit
        uint32      IndirectArgsOffset;         // v * NumDraws
        uint32      NumDraws;                   // Number of indirect slots owned by this view
    };

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

    // Device addresses of every scene buffer + bindless indices for the IBL/shadow textures. Built into
    // a per-view transient each frame; its address rides in FRootConstants.RootAddr. Must match
    // FSceneRoot in SceneGlobals.slang (128 B). No alignas: natural 8-byte packing matches Slang's
    // scalar field offsets (alignas(16) would pad the tail and break the size check).
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
        uint64 InstanceMeshletPrefix = 0;
        uint64 PreSkinnedVertices    = 0;  // GPU-written
        uint64 SkinDescriptors       = 0;
        uint64 Widgets               = 0;
        
        uint32 BRDFLutIndex          = 0;
        uint32 SkyIrradianceIndex    = 0;
        uint32 SkyPrefilterIndex     = 0;
        uint32 ShadowCascadeIndex    = 0;  // bindless 2D SRV (cascade atlas)
        uint32 ShadowAtlasIndex      = 0;  // bindless 2D SRV (spot/point atlas)
        uint32 SkyCubeIndex          = 0;  // bindless cube SRV (full-res sky; sharp near-mirror reflections)
    };
    static_assert(sizeof(FSceneRoot) == 128, "FSceneRoot must match SceneGlobals.slang");

    // The one engine-wide push constant. RootAddr -> FSceneRoot transient; PassAddr -> per-pass
    // constants transient (0 if the pass has none). Matches FRootConstants in SceneGlobals.slang.
    struct FRootConstants
    {
        uint64 RootAddr = 0;
        uint64 PassAddr = 0;
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
        
        FSSAOSettings   SSAOSettings;
        FCullData       CullData;
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
        uint8 bSSAO:1                   = false;
        uint8 bFrustumCull:1            = true;
        uint8 bOcclusionCull:1          = true;
        uint8 bShadowOcclusionCull:1    = true;
        uint8 bWireframe:1              = false;
        uint8 bDrawBillboards:1         = true;
        // Pre-upload reject of instances outside every contributing view.
        uint8 bCPUInstanceCull:1        = true;
        // Disabled = always LOD 0 (full detail).
        uint8 bUseLODs:1                = true;

        // Camera LOD + bias picks shadow LOD; capped at MAX_SHADOW_LOD. 0 = no saving, 1-2 typical.
        int8  ShadowLODBias             = 1;
    };
    
}
