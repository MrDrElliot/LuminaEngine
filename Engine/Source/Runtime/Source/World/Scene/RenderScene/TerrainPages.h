#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "World/ECS/Entity.h"
#include "TerrainRenderTypes.h"

namespace Lumina
{
    class FTerrainStore;

    constexpr int32  GTerrainPageQuads  = (int32)TERRAIN_PAGE_QUADS;
    constexpr int32  GTerrainPageGutter = (int32)TERRAIN_PAGE_GUTTER;
    constexpr int32  GTerrainPageTexels = (int32)TERRAIN_PAGE_TEXELS;
    constexpr int32  GTerrainPageTexelCount = GTerrainPageTexels * GTerrainPageTexels;
    constexpr uint32 GTerrainPageNone   = TERRAIN_PAGE_NONE;
    static_assert(GTerrainPageTexels == GTerrainPageQuads + 2 * GTerrainPageGutter, "A page slot is its quads plus a border each side.");
    static_assert(GTerrainPageQuads == GTerrainMeshletQuads << 3, "A page covers the node three levels above the leaves.");
    static_assert(GTerrainPageQuads == 1 << TERRAIN_PAGE_QUADS_LOG2, "Shaders shift by the page's quad count.");

    // 4096 slots of 72 texels, a 4608 texel atlas of about 170 MB across heights, normals and weights, enough for the rings of a few large terrains at once.
    constexpr uint32 GTerrainPageAtlasSlotsPerSide = TERRAIN_PAGE_ATLAS_SLOTS_SIDE;
    constexpr uint32 GTerrainPageAtlasSlots        = GTerrainPageAtlasSlotsPerSide * GTerrainPageAtlasSlotsPerSide;

    // A node draws up to twice its own split distance, so a mip stays resident a little past that.
    constexpr float  GTerrainPageRadiusScale = 3.0f;

    // About 4 MB of page data a frame, so a fresh view streams in over a few dozen frames, coarse pages first.
    constexpr uint32 GTerrainPageUploadsPerFrame = 128u;

    // Enough of the camera to predict which mips the quadtree will draw where.
    struct FTerrainPageView
    {
        FVector3 Camera        = FVector3(0.0f);
        float    PixelsPerUnit = 0.0f;
        bool     bOrthographic = false;
    };

    // Where a terrain sits, which turns its normalized node bounds into world boxes.
    struct FTerrainPageLayout
    {
        FVector2 OriginXZ      = FVector2(0.0f);
        float    WorldY        = 0.0f;
        float    SampleSpacing = 1.0f;
        float    MaxHeight     = 0.0f;
        int32    Resolution    = 0;
    };

    struct FTerrainPageRequest
    {
        uint32 Node     = 0u;
        int32  Mip      = 0;
        float  Distance = 0.0f;
    };

    namespace TerrainPages
    {
        // Every page the quadtree may draw from this view, parents before children.
        RUNTIME_API void SelectWanted(const FTerrainPageLayout& Layout, const TVector<FVector2>& NodeBounds, const FTerrainPageView& View,
                                      TVector<FTerrainPageRequest>& Out);

        // Heights as 16-bit UNORM, normals as RG8 XZ and the first four layer weights as RGBA8, border included and clamped at the map edge.
        RUNTIME_API void Build(const FTerrainStore& Store, const FTerrainPageLayout& Layout, uint32 Node, uint16* OutHeights, uint16* OutNormals, uint32* OutWeights);
    }

    // One terrain's page table, mirrored to the GPU by the renderer.
    struct FTerrainPageTable
    {
        TVector<uint32> Slots;
        TVector<uint8>  Stale;
        // Entries changed since the renderer last uploaded the table, as runs of one.
        TVector<FUIntVector2> DirtyRuns;
        int32 Resolution    = 0;
        float MaxHeight     = 0.0f;
        float TileWorldSize = 0.0f;
        bool  bReset        = true;
    };

    // Shared atlas residency for every terrain. The terrain extract decides and builds pages, then the renderer uploads them.
    class RUNTIME_API FTerrainPageStreamer
    {
    public:

        void BeginFrame();

        // A layout change drops every page, so stale data never shows at the wrong place.
        FTerrainPageTable& Prepare(ECS::FEntity Terrain, int32 Resolution, float MaxHeight, float TileWorldSize);

        // Resident pages whose samples or border touch the rect upload again.
        void MarkStale(ECS::FEntity Terrain, const FIntVector2& Min, const FIntVector2& Max);
        void MarkAllStale(ECS::FEntity Terrain);

        void Request(ECS::FEntity Terrain, const TSharedPtr<FTerrainStore>& Store, const FTerrainPageLayout& Layout, const FTerrainPageView& View);

        // Spends the frame's upload budget across all terrains, stale and coarse pages first.
        void Commit();

        void Prune(const TVector<ECS::FEntity>& Live);

        FTerrainPageTable* FindTable(ECS::FEntity Terrain) { return Tables.Find(Terrain); }

        // Filled by Commit and emptied by the renderer once uploaded, one page per slot entry.
        TVector<uint32> UploadSlots;
        TVector<uint16> UploadHeights;
        TVector<uint16> UploadNormals;
        TVector<uint32> UploadWeights;

    private:

        struct FSlot
        {
            ECS::FEntity Terrain;
            uint32       Node       = GTerrainPageNone;
            uint32       LastWanted = 0u;
        };

        struct FPending
        {
            ECS::FEntity              Terrain;
            TSharedPtr<FTerrainStore> Store;
            FTerrainPageLayout        Layout;
            FTerrainPageRequest       Request;
            bool                      bStale    = false;
        };

        uint32 AllocateSlot();
        void   FreeSlot(uint32 Slot);
        static void SetEntry(FTerrainPageTable& Table, uint32 Node, uint32 Slot);

        THashMap<ECS::FEntity, FTerrainPageTable> Tables;
        TVector<FSlot>    Slots;
        TVector<uint32>   FreeSlots;
        TVector<uint32>   EvictionOrder;
        TVector<FPending> Pending;
        TVector<FTerrainPageRequest> Wanted;
        uint32 Frame = 0u;
        bool   bEvictionOrderBuilt = false;
    };
}
