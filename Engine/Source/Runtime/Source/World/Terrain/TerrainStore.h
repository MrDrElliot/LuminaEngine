#pragma once

#include "Containers/Function.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Sync.h"
#include "Memory/SmartPtr.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"

namespace Lumina
{
    // One tile spans one page's 64 quads of its mip and stores 65 samples a side, its far edge shared with the next tile.
    constexpr int32 GTerrainTileSamples     = (int32)TERRAIN_PAGE_QUADS;
    constexpr int32 GTerrainTileShift       = (int32)TERRAIN_PAGE_QUADS_LOG2;
    constexpr int32 GTerrainTileEdge        = GTerrainTileSamples + 1;
    constexpr int32 GTerrainTileSampleCount = GTerrainTileEdge * GTerrainTileEdge;

    // Decoded tiles kept beyond those being edited, about 32 KB each with weights.
    constexpr int64 GTerrainStoreCacheBytes = 768ll << 20;

    // A terrain's heights and RGBA8 layer weights as a pyramid of tiles on its page nodes, read from a mapped file and edited in memory.
    class RUNTIME_API FTerrainStore
    {
    public:

        // Fills one row of Resolution normalized heights, rows arriving in order.
        using FRowSource = TFunction<void(int32 Row, float* OutHeights)>;

        struct FTile
        {
            TVector<float>  Heights;
            // Empty when nothing is painted, which reads as zero weight.
            TVector<uint32> Weights;
        };
        using FTileRef = TSharedPtr<const FTile>;

        // What an edit replaced, so undo puts the old tiles back and redo the new ones.
        struct FEdit
        {
            TVector<uint32>   Nodes;
            TVector<FTileRef> Before;
            TVector<FTileRef> After;
        };

        static TSharedPtr<FTerrainStore> CreateFlat(int32 Resolution, float Height01);
        static TSharedPtr<FTerrainStore> CreateFromArrays(int32 Resolution, TSpan<const float> Heights, TSpan<const uint8> LayerWeights, int32 LayerCount);
        static TSharedPtr<FTerrainStore> Open(FStringView DiskPath);
        // Streams rows into a file in one pass, so memory stays a few tile rows however large the map.
        static TSharedPtr<FTerrainStore> Import(FStringView DiskPath, int32 Resolution, const FRowSource& Rows);
        // Bilinear heights onto a new resolution, streamed to DiskPath, or kept in memory with nearest-sampled paint when it is empty.
        static TSharedPtr<FTerrainStore> Resample(const FTerrainStore& Source, int32 NewResolution, FStringView DiskPath);

        ~FTerrainStore();

        // Appends edited tiles to the open file, or writes a whole new one when the path differs.
        bool Save(FStringView DiskPath);

        int32  GetResolution() const   { return Resolution; }
        int32  GetTreeDepth() const    { return TreeDepth; }
        int32  GetPageDepth() const    { return PageDepth; }
        uint32 GetTileCount() const    { return TileCount; }
        bool   HasFile() const         { return File.IsValid(); }
        bool   IsDirty() const;
        // Drops every decoded tile the file can give back, for a store nothing draws or edits any more.
        void   ReleaseCleanTiles() const { TrimCache(0); }
        const FString& GetDiskPath() const { return DiskPath; }

        // Highest valid index of Mip's sample grid, past which reads repeat the map's last sample.
        int32 GetMipLast(int32 Mip) const;

        // Normalized heights over Mip's grid samples [Min, Min + Size), clamped at the map edge as the GPU pages are.
        void ReadHeights(int32 Mip, const FIntVector2& Min, const FIntVector2& Size, float* Out) const;
        void ReadWeights(int32 Mip, const FIntVector2& Min, const FIntVector2& Size, uint32* Out) const;
        float GetHeight(int32 X, int32 Y) const;
        // Bilinear between finest samples, at a position in samples.
        float SampleHeight(float X, float Y) const;

        // Finest samples, after which the coarser mips and the bounds over the rect follow.
        void WriteHeights(const FIntVector2& Min, const FIntVector2& Size, const float* In);
        void WriteWeights(const FIntVector2& Min, const FIntVector2& Size, const uint32* In);
        // Weight channel i of every painted tile takes channel Source[i], or zero when Source[i] is negative.
        void RemapLayers(const int32 (&Source)[GTerrainMaxLayers]);

        // Records the finest tiles writes replace until EndEdit, for one undoable stroke.
        void   BeginEdit();
        FEdit  EndEdit();
        void   ApplyEdit(const FEdit& Edit, bool bUndo);

        // Normalized min and max over every finest sample under each page node, root first.
        const TVector<FVector2>& GetBounds() const { return Bounds; }
        // Bumped on every bounds change, with the nodes that changed since Serial, or false when the log no longer reaches back that far.
        uint64 GetChangeSerial() const { return ChangeSerial; }
        // Bumped on every write and undo, bounds changed or not.
        uint64 GetEditSerial() const { return EditSerial; }
        bool   GetChangedNodes(uint64 Serial, TVector<uint32>& OutNodes) const;

    private:

        FTerrainStore() = default;
        void InitLayout(int32 InResolution);

        struct FTileEntry
        {
            uint64 Offset      = 0;
            uint32 HeightBytes = 0;
            uint32 WeightBytes = 0;
        };

        struct FCachedTile
        {
            FTileRef Tile;
            uint64   LastUse = 0;
            bool     bDirty  = false;
        };

        uint32 NodeIndex(int32 Depth, int32 X, int32 Y) const;
        // No quads start in it, so its bounds stay empty and the cull skips it.
        bool   IsTileEmpty(int32 Depth, int32 X, int32 Y) const;
        // Holds samples, which includes a tile starting on the map's last sample, since its neighbors read that sample.
        bool   HasTileData(int32 Depth, int32 X, int32 Y) const;
        FTileRef GetTile(uint32 Node) const;
        FTileRef DecodeTile(uint32 Node) const;
        void   PublishTile(uint32 Node, FTileRef Tile, bool bDirty);
        void   TrimCache(int64 Budget) const;
        FTileRef BuildCoarserTile(int32 Depth, int32 X, int32 Y) const;
        void   RefreshFrom(const FIntVector2& Min, const FIntVector2& Max);
        void   RefreshBounds(const FIntVector2& Min, const FIntVector2& Max);
        void   NoteChanged(uint32 Node);
        bool   WriteFile(FStringView Path);
        bool   AppendDirty();
        bool   MapFile(FStringView Path);
        const FTileEntry* Directory() const;
        void   EncodeTile(const FTile& Tile, TVector<uint8>& OutHeights, TVector<uint8>& OutWeights) const;

        int32  Resolution = 0;
        int32  TreeDepth  = 0;
        int32  PageDepth  = 0;
        uint32 TileCount  = 0;
        FString DiskPath;

        Filesystem::FMappedFile File;
        TVector<FVector2> Bounds;

        // Decoders read the mapping under a shared lock, and saving takes it exclusively to remap.
        mutable FSharedMutex FileLock;
        // Held only to look up or publish a tile, never while decoding one.
        mutable FMutex CacheLock;
        mutable THashMap<uint32, FCachedTile> Cache;
        mutable uint64 UseClock   = 0;
        mutable int64  CacheBytes = 0;

        bool    bEditing = false;
        FEdit   Edit;
        THashMap<uint32, int32> EditSlot;

        uint64 ChangeSerial = 0;
        uint64 EditSerial   = 0;
        uint64 ChangeLogBase = 0;
        TVector<uint32> ChangeLog;
    };
}
