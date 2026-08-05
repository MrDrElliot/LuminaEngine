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

    // Index into FMeshResolveCache. Lives here rather than beside the cache so component headers can hold
    // one without pulling in the whole resolve machinery.
    constexpr uint32 INVALID_MESH_RESOLVE_HANDLE = ~0u;

    /**
     * Staleness token a consumer caches to decide whether its copy of a resolve entry is still current.
     *
     * A live entry's token is (FResolvedMesh::Generation << 1), so bit 0 is never set on one and neither
     * sentinel below can be mistaken for a real token. Comparing tokens is what replaced the old global
     * epoch stamp: an epoch bump is a scene-wide invalidation, and one mesh finishing its GPU upload is
     * not one of those.
     */
    constexpr uint32 MESH_RESOLVE_STATE_STALE   = ~0u;  // never resolved, or invalidated since
    constexpr uint32 MESH_RESOLVE_STATE_NO_MESH = 0u;   // settled: there is nothing to resolve


    // Mirror of MESHLET_DRAW_INDEX_BITS in Common.slang. FMeshletDraw packs the meshlet-local index into
    // this many bits and spends the rest on the frame tag, so no single surface LOD may hold more meshlets
    // than the field can address -- past it the index wraps and silently resolves the wrong meshlet, which
    // is the precise failure mode the tag exists to prevent.
    constexpr uint32 MESHLET_DRAW_INDEX_BITS   = 20;
    constexpr uint32 MAX_MESHLETS_PER_SURFACE_LOD = (1u << MESHLET_DRAW_INDEX_BITS);

    /**
     * Hard ceiling on the meshlet cull's (instance, meshlet) work domain -- the size of an INDIRECT
     * dispatch, so the last place a bad number can be stopped before it reaches the GPU.
     *
     * 2^28 threads is far past any real scene (it is ~4.2M workgroups of 64) and still finishes in well
     * under a frame, whereas the value one bad input produces is 2^32 -- 65535 x 65535 groups, which the
     * GPU chews on until the TDR fires. That failure reports as VK_ERROR_DEVICE_LOST with NO page fault
     * and a running wave parked in CullMeshlets, because nothing ever touched a bad address: the grid was
     * simply unbounded. Sized as a sanity limit, never as a resource bound -- see the use site.
     */
    constexpr uint32 GMaxMeshletCullDomain = (1u << 28);

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
            case ERenderSceneDebugFlags::SSAO:              return "SSAO";
            case ERenderSceneDebugFlags::MaterialID:        return "Material ID";
            case ERenderSceneDebugFlags::TriangleID:        return "Triangle ID";
            case ERenderSceneDebugFlags::OITAccumColor:     return "OIT Accum Color";
            case ERenderSceneDebugFlags::OITAccumWeight:    return "OIT Accum Weight";
            case ERenderSceneDebugFlags::OITRevealage:      return "OIT Revealage";
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

        // Retained cull entries only; meaningless on a compacted output instance. Both live here rather
        // than in fields of their own because FInstanceCullEntry has to stay 32 bytes, and both are
        // needed BEFORE the survivor payload is loaded.
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
    // ReuseSampledSlot adopts a slot detached from a retiring image (see DetachSampledSlot) rather
    // than allocating a fresh one, so the index stays valid for anyone who cached it.
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

    // A non-owning copy of an image owned elsewhere. Use this whenever an image is handed to a second
    // holder (e.g. a view snapshotting the scene's shared IBL cubes): the copy reads identically but
    // is skipped by ownership-driven release, so only the real owner ever frees it.
    NODISCARD inline FSceneImage BorrowSceneImage(const FSceneImage& Owner)
    {
        FSceneImage Copy = Owner;
        Copy.bOwned = false;
        return Copy;
    }

    // Take the sampled slot off a retiring image so its release destroys the texture only. The slot
    // then belongs to the caller, who is expected to hand it straight to the replacement image via
    // CreateSceneImage's ReuseSampledSlot -- dropping it on the floor leaks a heap slot.
    //
    // This exists because a bindless ResourceID is a bare uint32 with no ownership: once it has been
    // published outside the renderer (the editor puts the view's Output index into ImGui draw data),
    // the renderer can no longer tell when the last holder is done with it. Keeping the slot alive
    // across a resize sidesteps the question entirely -- a stale index resolves to the NEW image
    // rather than to a freed one.
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
        // A mip slot is invalid when the RW heap was full at create time, so it needs the same
        // guard the sampled slot gets above: freeing kInvalidHeapSlot indexes the heap out of bounds.
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
        uint32          StartVertex;
        uint32          VertexCount;
        ESolidDrawMode  Mode;
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

    // One local reflection probe. Extraction bakes the half-extents into WorldToProbe so the influence
    // volume is the unit box [-1,1]^3 (or the unit sphere) in probe space: the inside test, the blend
    // weight, and the parallax ray-volume intersection then all share one space and need no extents.
    // A ray-volume intersection is invariant under the linear part of the transform, so the reflection
    // direction can be carried into probe space unnormalized and the resulting t used directly.
    // Must match FGPUReflectionProbe in ReflectionProbe.slang.
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

    // CPU-only capture parameters for one probe, held parallel to the GPU probe array. Never uploaded:
    // the bake reads it to build the six face cameras, and nothing on the GPU needs it.
    struct FReflectionProbeCapture
    {
        FVector3 Position  = FVector3(0.0f);   // world-space capture origin (entity origin + CaptureOffset)
        float    NearPlane = 0.1f;
        float    FarPlane  = 500.0f;
        uint32   FaceSize  = 128u;             // per-probe capture resolution tier
        // EReflectionProbeUpdateMode::Always. Deliberately NOT part of the change comparison that
        // invalidates bakes: toggling it should start or stop refreshing, not throw away every slice.
        bool     bAlwaysUpdate = false;
        // EReflectionProbeClearMode::SolidColor: fill uncovered directions with ClearColor instead of
        // rendering the sky into them.
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

    /**
     * Per-surface LOD table, so LOD selection can run on the GPU instead of the CPU gather.
     *
     * Interned per distinct resolved surface: every instance of one rock shares a single entry, so the
     * table is sized by the scene's distinct (mesh, material) surfaces rather than by instance count.
     * Retained -- uploaded when a surface is bound, never per frame.
     */
    struct alignas(16) FSurfaceDescGPU
    {
        uint32  LODMeshletOffset[MAX_MESH_LODS];
        uint32  LODMeshletCount[MAX_MESH_LODS];
        // Squared, matching the CPU's SelectLODIndex: compares DistSq against Threshold^2 * RadiusSq,
        // so neither side needs a square root.
        float   LODScreenThresholdSq[MAX_MESH_LODS];
        uint32  NumLODs;
        uint32  _Pad;
    };
    static_assert(sizeof(FSurfaceDescGPU) == 80, "FSurfaceDescGPU layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FSurfaceDescGPU)

    /**
     * A 4x4 AFFINE transform with the always-(0,0,0,1) last row dropped: 48 B instead of 64.
     *
     * Blends linearly like the matrix; transform a point with explicit row dot products. Used for both
     * skinning matrices and retained instance transforms -- one convention, so there is one place to get
     * it wrong rather than two.
     *
     * The convention, verified against the generated SPIR-V rather than assumed:
     *  - TMat is COLUMN-MAJOR, `M[c]` is column c, so `M[c][r]` is mathematical element (r, c).
     *  - Slang stores float4x4 as `_MatrixStorage_float4x4_ColMajor`, i.e. mathematical element (r, c) at
     *    float index 4*c + r -- IDENTICAL to TMat. The two agree on the matrix; there is no transpose.
     *  - Slang's `float4x4(v0, v1, v2, v3)` fills ROWS (probe: the ctor + `mul(M, e0)` constant-folded to
     *    column 0 of a row-filled matrix). So the shader rebuilds this with
     *    `float4x4(Row0, Row1, Row2, float4(0,0,0,1))`.
     */
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

    /**
     * The retained scene's CULL-HOT half: everything CullInstances.slang reads about a slot it is about
     * to REJECT, and nothing else.
     *
     * 32 bytes, so two slots share a cache line. That pass runs one lane per retained slot, over EVERY
     * slot, EVERY frame. It used to load the whole 144-byte FGPUInstance plus a 16-byte cull record to
     * consume about 44 bytes of them -- pulling ~5x the bandwidth it needed through the memory system,
     * at scenes north of 900k surfaces.
     *
     * What a SURVIVOR needs lives in the parallel transform array and FInstanceStatic, neither of which
     * is touched until this entry says the instance is visible.
     */
    struct alignas(16) FInstanceCullEntry
    {
        // NO empty ctor, unlike FGPUInstance. emplace_back must VALUE-initialize this, so a slot that is
        // allocated but not yet written reads as inactive rather than as garbage flags that the cull
        // could take for a live instance.
        FVector4    SphereBounds;       // world space; xyz = center, w = radius
        uint32      DrawIDAndFlags;     // PackDrawIDAndFlags; carries Active and HasGeometry
        uint32      SurfaceDescIndex;   // into the interned LOD tables
        float       MaxDrawDistance;    // 0 = never distance-culled
        int32       ForcedLODIndex;     // -1 = automatic (distance / radius)
    };
    static_assert(sizeof(FInstanceCullEntry) == 32, "FInstanceCullEntry layout must match shader");
    VERIFY_SSBO_ALIGNMENT(FInstanceCullEntry)

    /**
     * The retained scene's COLD half: what a SURVIVING instance needs copied into the compacted output,
     * and which changes only when the primitive is re-bound -- never when it merely moves.
     *
     * Split out for both sides. The cull never reads it for a slot it rejects, and a transform update on
     * the CPU writes 16 bytes of bounds plus one matrix instead of restamping all of this too.
     */
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

    // 144B per-instance descriptor. This is the COMPACTED OUTPUT layout only -- CullInstances assembles
    // one of these per survivor from the three retained arrays above, and every downstream pass reads it
    // per meshlet / vertex / pixel with high per-instance locality, which is what AoS is good at.
    // Empty ctor skips zero-init on resize() (parallel writer overwrites everything).
    //
    // NO alignas(16). The layout is SCALAR (Slang emits this as `_natural`), so alignment comes from the
    // widest member -- 8, for MeshletHeaderAddress -- and the stride is the plain sum of the members.
    // Forcing 16 here would round sizeof up to 128 while Slang keeps computing 120, and the two strides
    // would silently disagree one element in.
    struct FGPUInstance
    {
        FGPUInstance() noexcept {}

        // 3x4, not 4x4: an instance transform is always affine. The shader expands it through the
        // ModelMatrix property, so readers are unchanged.
        FTransform3x4   Transform;              // offset   0
        FVector4        SphereBounds;           //         48
        // Kept adjacent to the 16-byte-aligned members so it lands 8-aligned, which scalar layout
        // requires of a 64-bit member and which sets the struct's alignment.
        uint64          MeshletHeaderAddress;   //         64

        uint32          DrawIDAndFlags;         //         72
        uint32          SurfaceMeshletOffset;   //         76
        uint32          SurfaceMeshletCount;    //         80
        uint32          ShadowMeshletOffset;    //         84
        uint32          ShadowMeshletCount;     //         88
        uint32          CustomData;             //         92

        // Full 32-bit bone index (was packed into 16 bits with MaterialIndex, which capped
        // the scene at 64k total bones).
        uint32          BoneOffset;             //         96
        uint32          MaterialIndex;          //        100
        uint32          EntityID;               //        104
        // Base index into the pre-skinned vertex buffer for this instance's surface-LOD meshlet block;
        // the skinning pass writes there, the draw VS reads instead of re-skinning. 0 for static
        // instances, kNoPreSkinBase when the entity lost the pre-skin budget.
        uint32          SkinnedVertexBase;      //        108
        // Same, for the shadow-LOD block. Equal to SkinnedVertexBase when the shadow LOD resolved to
        // the same block, which is the common case; the two differ only when the LODs diverge.
        uint32          ShadowSkinnedVertexBase;//        112
        // Declared, not implied: under scalar layout the shader's array stride comes from the
        // declared members, so trailing padding has to exist on both sides or the strides diverge.
        // ONE word, not three: 116 rounds to 120 against the 8-byte alignment the pointer imposes.
        uint32          _Pad;                   //        116
    };

    // Both verified against what Slang actually emits for this struct, not merely asserted here:
    // `slangc -target spirv-asm` reports ArrayStride 120 and member offsets 0/48/64/72/76/80/84/88/92/
    // 96/100/104/108/112/116, matching the declaration order above exactly.
    static_assert(sizeof(FGPUInstance) == 120, "FGPUInstance layout must match shader");
    static_assert(alignof(FGPUInstance) == 8, "Scalar layout: alignment comes from MeshletHeaderAddress");

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
    // DrawID in the low 16 bits, flags in the high 16. Batches number in the tens to hundreds, so 16 bits
    // of draw id is enormous headroom -- and widening the flag field is what gives FInstanceCullEntry
    // somewhere to put Active and HasGeometry without a 33rd byte. Mirrored by Common.slang's accessors.
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
        FGPUFrustum Frustum;
        FGPUFrustum ShadowFrustum;

        // Per-cascade CASTER volume: the camera sub-frustum slice that cascade i actually shades, swept
        // toward the sun. This is NOT the cascade's own ortho box -- that box is fitted to a sphere around
        // the slice, and because the sphere's radius is dominated by the slice's lateral extent, the outer
        // boxes fully CONTAIN the inner ones. Culling on the box alone therefore put nearly every caster in
        // every cascade. The slice volume does not nest, so a caster lands only in the cascades that shade it.
        FGPUFrustum CascadeFrustum[NumCascades];

        // Cascade Hi-Z reprojection. These describe the cascade transforms that produced the CURRENT
        // contents of the cascade pyramid, i.e. LAST frame's -- the pyramid is built after the shadow
        // raster, so this frame's cull can only test against the previous one.
        FMatrix4 CascadeHZBViewProjection[NumCascades];
        // xy = pyramid-UV offset of the cascade's tile, zw = its UV scale.
        FVector4 CascadeHZBTile[NumCascades];
        // xyz = NDC units per world unit for that cascade's ortho volume (x/y = 1/Radius, z = 1/OrthoRange).
        // Avoids reconstructing the scale from the matrix rows in the shader.
        FVector4 CascadeHZBNdcScale[NumCascades];

        uint32 bFrustumCull;
        uint32 bOcclusionCull;
        uint32 InstanceNum;
        uint32 bHasDirectional;

        float PyramidWidth;
        float PyramidHeight;

        float  ShadowMaxDistance;
        uint32 bShadowOcclusionCull;

        // Tag the meshlet draw list is stamped with this frame; see FMeshletDraw in Common.slang.
        // Never 0, so an unwritten (zeroed) entry can never be mistaken for a live one.
        uint32 MeshletDrawTag;
        uint32 DebugMode;
        // Entries in the shared meshlet draw list. This is the ALLOCATION's capacity, not the frame's
        // actual meshlet total -- that total is produced by BuildDrawPrefix and never reaches the CPU.
        // Consumers want it as a fault-safe upper bound on a draw-list index, which is exactly what a
        // capacity is.
        uint32 MeshletDrawListCapacity;
        // Bindless ResourceID of the depth pyramid; HZB tap goes through uBindlessTex2D.
        uint32 DepthPyramidIndex;

        // Cascade Hi-Z pyramid: MAX-reduced (the cascade atlas is standard-Z, unlike the camera's
        // reverse-Z pyramid). 0 in bCascadeHZBValid means nothing has been rendered into it yet and every
        // cascade occlusion test must pass.
        uint32 CascadePyramidIndex;
        uint32 bCascadeHZBValid;
        float  CascadePyramidWidth;
        float  CascadePyramidHeight;
        uint32 CascadePyramidMipCount;
        // Entries in Bones(). Same meaning as MeshletDrawListCapacity above: a fault-safe upper bound on
        // a bone index, not a count anything iterates. SkinVertex indexes with BoneOffset + a joint index
        // read out of vertex data, so a mesh skinned against the wrong skeleton indexes off the end --
        // a device-lost page fault rather than a wrong pose, and the importer already warns about
        // exactly that case ("channels target bones missing from the skeleton").
        //
        // Takes a pad slot so the struct size, and the layout Common.slang mirrors, are unchanged.
        uint32 BoneNum;
        uint32 _CullPad1;
        uint32 _CullPad2;
    };

    VERIFY_SSBO_ALIGNMENT(FCullData)

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
            // CSM cascade: also test CullData.CascadeFrustum[CascadeIndex] (the tight, non-nested caster
            // volume) and the cascade Hi-Z, and reject bounds smaller than MinBoundsDiameter.
            Cascade         = BIT(7),
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
        // These two were the draw list's per-view slice before it became a GPU-packed per-(view, draw)
        // region; they were left as reserved padding to hold the 128-byte stride. The cascade cull reuses
        // them rather than growing the struct.
        uint32      CascadeIndex;               // Cascade this view rasterizes; only read when Flags has Cascade
        float       MinBoundsDiameter;          // Reject bounds thinner than this (world units); 0 = off
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
        uint64 ReflectionProbes      = 0;  // FGPUReflectionProbe array, sorted by descending priority

        uint32 BRDFLutIndex          = 0;
        uint32 SkyIrradianceIndex    = 0;
        uint32 SkyPrefilterIndex     = 0;
        uint32 ShadowCascadeIndex    = 0;  // bindless 2D SRV (cascade atlas)
        uint32 ShadowAtlasIndex      = 0;  // bindless 2D SRV (spot/point atlas)
        uint32 SkyCubeIndex          = 0;  // bindless cube SRV (full-res sky; sharp near-mirror reflections)
        // [31:24] = probe prefilter mip count (mirrors the SkyPrefilterIndex packing), [23:0] = bindless
        // cube-array SRV holding every probe's prefiltered radiance.
        uint32 ProbeCubeArrayIndex   = 0;
        uint32 NumReflectionProbes   = 0;
    };
    static_assert(sizeof(FSceneRoot) == 144, "FSceneRoot must match SceneGlobals.slang");

    // The one engine-wide push constant. RootAddr -> FSceneRoot transient; PassAddr -> per-pass
    // constants transient (0 if the pass has none). Matches FRootConstants in SceneGlobals.slang.
    struct FRootConstants
    {
        uint64 RootAddr = 0;
        uint64 PassAddr = 0;
    };

    // Global scaling applied on top of every material's authored Parallax Occlusion Mapping settings,
    // so quality can be dialed back at runtime without recompiling materials. Driven by the r.POM.*
    // CVars; must match FParallaxSettings in Common.slang.
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

        FSSAOSettings   SSAOSettings;
        FCullData       CullData;
        FParallaxSettings ParallaxSettings;
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
        uint8 bConeCull:1               = true;
        uint8 bOcclusionCull:1          = true;
        uint8 bShadowOcclusionCull:1    = true;
        uint8 bWireframe:1              = false;
        uint8 bDrawBillboards:1         = true;
        uint8 bCPUInstanceCull:1        = true;
        uint8 bUseLODs:1                = true;
        int8  ShadowLODBias             = 1;
        float ShadowCoarseLODDistance   = 150.0f;
    };
    
}
