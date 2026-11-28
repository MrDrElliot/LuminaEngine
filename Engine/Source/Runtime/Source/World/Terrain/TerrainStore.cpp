#include "RuntimePCH.h"
#include "TerrainStore.h"
#include "Containers/Algorithm.h"
#include "Core/Templates/NumericLimits.h"
#include "TaskSystem/TaskSystem.h"
#include "zstd.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kStoreMagic   = 0x5245544Cu;
        constexpr uint32 kStoreVersion = 1u;
        // Fast enough to keep a streaming import disk bound, and heights already delta coded compress well at it.
        constexpr int    kZstdLevel    = 1;
        // Tiles encoded per parallel batch on save, which bounds the encoded bytes held at once.
        constexpr uint32 kSaveBatch    = 2048u;
        // Renderers that fall this far behind upload every bound again instead.
        constexpr int32  kMaxChangeLog = 1 << 18;

        struct FStoreHeader
        {
            uint32 Magic      = kStoreMagic;
            uint32 Version    = kStoreVersion;
            int32  Resolution = 0;
            int32  PageDepth  = 0;
            uint32 TileCount  = 0;
            uint32 _Pad       = 0;
            uint64 BoundsOffset    = 0;
            uint64 DirectoryOffset = 0;
            uint64 DataOffset      = 0;
        };
        static_assert(sizeof(FStoreHeader) == 48);

        const FVector2 kEmptyBounds = FVector2(TNumericLimits<float>::Infinity(), -TNumericLimits<float>::Infinity());

        uint16 Quantize(float Height01)
        {
            return (uint16)(Math::Clamp(Height01, 0.0f, 1.0f) * 65535.0f + 0.5f);
        }

        // Row deltas, since neighbors differ by little and the small values compress several times better.
        void DeltaEncode(const float* Heights, uint16* Out)
        {
            for (int32 Y = 0; Y < GTerrainTileEdge; ++Y)
            {
                uint16 Previous = 0;
                for (int32 X = 0; X < GTerrainTileEdge; ++X)
                {
                    const uint16 Value = Quantize(Heights[Y * GTerrainTileEdge + X]);
                    Out[Y * GTerrainTileEdge + X] = (uint16)(Value - Previous);
                    Previous = Value;
                }
            }
        }

        void DeltaDecode(const uint16* In, float* Heights)
        {
            constexpr float kInvMax = 1.0f / 65535.0f;
            for (int32 Y = 0; Y < GTerrainTileEdge; ++Y)
            {
                uint16 Value = 0;
                for (int32 X = 0; X < GTerrainTileEdge; ++X)
                {
                    Value = (uint16)(Value + In[Y * GTerrainTileEdge + X]);
                    Heights[Y * GTerrainTileEdge + X] = float(Value) * kInvMax;
                }
            }
        }

        bool Compress(const void* Data, size_t Bytes, TVector<uint8>& Out)
        {
            Out.SetNum((int32)ZSTD_compressBound(Bytes));
            const size_t Written = ZSTD_compress(Out.GetData(), (size_t)Out.Num(), Data, Bytes, kZstdLevel);
            if (ZSTD_isError(Written))
            {
                Out.Reset();
                return false;
            }
            Out.SetNum((int32)Written);
            return true;
        }

        FVector2 Union(const FVector2& A, const FVector2& B)
        {
            return FVector2(Math::Min(A.x, B.x), Math::Max(A.y, B.y));
        }

        // The tile holding a grid sample, where the far edge of the last tile is that tile's own, and the sample's spot in it.
        FORCEINLINE int32 TileOf(int32 Sample, int32 Side)
        {
            return Math::Min(Sample >> GTerrainTileShift, Side - 1);
        }

        // Tiles touching grid samples [Lo, Hi], including the one before whose shared far edge is Lo.
        FORCEINLINE FIntVector2 TilesTouching(int32 Lo, int32 Hi, int32 Side)
        {
            return FIntVector2(Math::Max(0, (Lo - 1) >> GTerrainTileShift), Math::Min(Side - 1, Hi >> GTerrainTileShift));
        }
    }

    FTerrainStore::~FTerrainStore() = default;

    void FTerrainStore::InitLayout(int32 InResolution)
    {
        Resolution = Math::Max(InResolution, 2);
        TreeDepth  = GetTerrainTreeDepth(Resolution);
        PageDepth  = GetTerrainPageDepth(TreeDepth);
        TileCount  = GetTerrainNodeCount(PageDepth);
        Bounds.Init(kEmptyBounds, (int32)TileCount);
    }

    int32 FTerrainStore::GetMipLast(int32 Mip) const
    {
        const int32 Last = Resolution - 1;
        return (Last + (1 << Mip) - 1) >> Mip;
    }

    uint32 FTerrainStore::NodeIndex(int32 Depth, int32 X, int32 Y) const
    {
        return GetTerrainNodeDepthOffset(Depth) + (uint32)Y * (1u << Depth) + (uint32)X;
    }

    bool FTerrainStore::IsTileEmpty(int32 Depth, int32 X, int32 Y) const
    {
        const int32 Shift = GTerrainTileShift + (PageDepth - Depth);
        const int64 Last  = Resolution - 1;
        return (int64(X) << Shift) >= Last || (int64(Y) << Shift) >= Last;
    }

    bool FTerrainStore::HasTileData(int32 Depth, int32 X, int32 Y) const
    {
        const int32 MipLast = GetMipLast(PageDepth - Depth);
        return (X << GTerrainTileShift) <= MipLast && (Y << GTerrainTileShift) <= MipLast;
    }

    const FTerrainStore::FTileEntry* FTerrainStore::Directory() const
    {
        if (!File.IsValid())
        {
            return nullptr;
        }
        const FStoreHeader* Header = reinterpret_cast<const FStoreHeader*>(File.Data());
        return reinterpret_cast<const FTileEntry*>(File.Data() + Header->DirectoryOffset);
    }

    FTerrainStore::FTileRef FTerrainStore::DecodeTile(uint32 Node) const
    {
        FReadScopeLock Guard(FileLock);
        const FTileEntry* Entries = Directory();
        if (Entries == nullptr || Node >= TileCount || Entries[Node].HeightBytes == 0)
        {
            return nullptr;
        }
        const FTileEntry& Entry = Entries[Node];

        uint16 Deltas[GTerrainTileSampleCount];
        const size_t HeightBytes = ZSTD_decompress(Deltas, sizeof(Deltas), File.Data() + Entry.Offset, Entry.HeightBytes);
        if (ZSTD_isError(HeightBytes) || HeightBytes != sizeof(Deltas))
        {
            LOG_ERROR("Terrain store {}: tile {} has corrupt heights.", DiskPath, Node);
            return nullptr;
        }

        TSharedPtr<FTile> Tile = MakeShared<FTile>();
        Tile->Heights.SetNum(GTerrainTileSampleCount);
        DeltaDecode(Deltas, Tile->Heights.GetData());
        if (Entry.WeightBytes != 0)
        {
            Tile->Weights.SetNum(GTerrainTileSampleCount);
            const size_t WeightBytes = ZSTD_decompress(Tile->Weights.GetData(), GTerrainTileSampleCount * sizeof(uint32),
                                                       File.Data() + Entry.Offset + Entry.HeightBytes, Entry.WeightBytes);
            if (ZSTD_isError(WeightBytes) || WeightBytes != GTerrainTileSampleCount * sizeof(uint32))
            {
                LOG_ERROR("Terrain store {}: tile {} has corrupt weights.", DiskPath, Node);
                Tile->Weights.Reset();
            }
        }
        return Tile;
    }

    FTerrainStore::FTileRef FTerrainStore::GetTile(uint32 Node) const
    {
        {
            TScopeLock Guard(CacheLock);
            if (FCachedTile* Cached = Cache.Find(Node))
            {
                Cached->LastUse = ++UseClock;
                return Cached->Tile;
            }
        }

        FTileRef Decoded = DecodeTile(Node);
        if (Decoded == nullptr)
        {
            return nullptr;
        }

        TScopeLock Guard(CacheLock);
        FCachedTile& Slot = Cache.FindOrAdd(Node);
        if (Slot.Tile == nullptr)
        {
            Slot.Tile = Decoded;
            CacheBytes += int64(Decoded->Heights.Num() * sizeof(float) + Decoded->Weights.Num() * sizeof(uint32));
        }
        Slot.LastUse = ++UseClock;
        FTileRef Result = Slot.Tile;
        TrimCache(GTerrainStoreCacheBytes);
        return Result;
    }

    // Clean tiles reload from the file, so only a file-backed store evicts, oldest first, and never a tile being edited.
    void FTerrainStore::TrimCache(int64 Budget) const
    {
        if (CacheBytes <= Budget || !File.IsValid())
        {
            return;
        }
        TVector<TPair<uint64, uint32>> Clean;
        for (const auto& [Node, Entry] : Cache)
        {
            if (!Entry.bDirty)
            {
                Clean.Add({ Entry.LastUse, Node });
            }
        }
        Algo::Sort(Clean, [](const TPair<uint64, uint32>& A, const TPair<uint64, uint32>& B) { return A.Key < B.Key; });
        const int64 Target = Budget - Budget / 8;
        for (const TPair<uint64, uint32>& Victim : Clean)
        {
            if (CacheBytes <= Target)
            {
                break;
            }
            if (const FCachedTile* Entry = Cache.Find(Victim.Value))
            {
                CacheBytes -= int64(Entry->Tile->Heights.Num() * sizeof(float) + Entry->Tile->Weights.Num() * sizeof(uint32));
            }
            Cache.Remove(Victim.Value);
        }
    }

    void FTerrainStore::PublishTile(uint32 Node, FTileRef Tile, bool bDirty)
    {
        TScopeLock Guard(CacheLock);
        FCachedTile& Slot = Cache.FindOrAdd(Node);
        if (Slot.Tile != nullptr)
        {
            CacheBytes -= int64(Slot.Tile->Heights.Num() * sizeof(float) + Slot.Tile->Weights.Num() * sizeof(uint32));
        }
        Slot.Tile    = Tile;
        Slot.LastUse = ++UseClock;
        Slot.bDirty  = Slot.bDirty || bDirty;
        if (Tile != nullptr)
        {
            CacheBytes += int64(Tile->Heights.Num() * sizeof(float) + Tile->Weights.Num() * sizeof(uint32));
        }
    }

    void FTerrainStore::ReadHeights(int32 Mip, const FIntVector2& Min, const FIntVector2& Size, float* Out) const
    {
        const int32 Depth   = PageDepth - Mip;
        const int32 Side    = 1 << Depth;
        const int32 MipLast = GetMipLast(Mip);
        uint32   LastNode = ~0u;
        FTileRef Tile;
        for (int32 Y = 0; Y < Size.y; ++Y)
        {
            const int32 GY = Math::Clamp(Min.y + Y, 0, MipLast);
            const int32 TY = TileOf(GY, Side);
            for (int32 X = 0; X < Size.x; ++X)
            {
                const int32  GX   = Math::Clamp(Min.x + X, 0, MipLast);
                const int32  TX   = TileOf(GX, Side);
                const uint32 Node = NodeIndex(Depth, TX, TY);
                if (Node != LastNode)
                {
                    Tile     = GetTile(Node);
                    LastNode = Node;
                }
                const int32 Local = (GY - (TY << GTerrainTileShift)) * GTerrainTileEdge + (GX - (TX << GTerrainTileShift));
                Out[Y * Size.x + X] = Tile != nullptr ? Tile->Heights[Local] : 0.0f;
            }
        }
    }

    void FTerrainStore::ReadWeights(int32 Mip, const FIntVector2& Min, const FIntVector2& Size, uint32* Out) const
    {
        const int32 Depth   = PageDepth - Mip;
        const int32 Side    = 1 << Depth;
        const int32 MipLast = GetMipLast(Mip);
        uint32   LastNode = ~0u;
        FTileRef Tile;
        for (int32 Y = 0; Y < Size.y; ++Y)
        {
            const int32 GY = Math::Clamp(Min.y + Y, 0, MipLast);
            const int32 TY = TileOf(GY, Side);
            for (int32 X = 0; X < Size.x; ++X)
            {
                const int32  GX   = Math::Clamp(Min.x + X, 0, MipLast);
                const int32  TX   = TileOf(GX, Side);
                const uint32 Node = NodeIndex(Depth, TX, TY);
                if (Node != LastNode)
                {
                    Tile     = GetTile(Node);
                    LastNode = Node;
                }
                const int32 Local = (GY - (TY << GTerrainTileShift)) * GTerrainTileEdge + (GX - (TX << GTerrainTileShift));
                Out[Y * Size.x + X] = Tile != nullptr && !Tile->Weights.IsEmpty() ? Tile->Weights[Local] : 0u;
            }
        }
    }

    float FTerrainStore::GetHeight(int32 X, int32 Y) const
    {
        float Height = 0.0f;
        ReadHeights(0, FIntVector2(X, Y), FIntVector2(1, 1), &Height);
        return Height;
    }

    float FTerrainStore::SampleHeight(float X, float Y) const
    {
        const float Last = float(Resolution - 1);
        X = Math::Clamp(X, 0.0f, Last);
        Y = Math::Clamp(Y, 0.0f, Last);
        const int32 X0 = (int32)X;
        const int32 Y0 = (int32)Y;
        float Corners[4];
        ReadHeights(0, FIntVector2(X0, Y0), FIntVector2(2, 2), Corners);
        const float TX = X - float(X0);
        const float TY = Y - float(Y0);
        return Math::Mix(Math::Mix(Corners[0], Corners[1], TX), Math::Mix(Corners[2], Corners[3], TX), TY);
    }

    // Heights decimate so coarse vertices land exactly on fine ones, and weights average two by two so far paint does not alias.
    FTerrainStore::FTileRef FTerrainStore::BuildCoarserTile(int32 Depth, int32 X, int32 Y) const
    {
        // Two finer samples per coarse one, plus the extra row and column the far edge's weight average reads.
        constexpr int32 kFine = GTerrainTileEdge * 2;
        const int32 FinerMip = PageDepth - Depth - 1;
        const FIntVector2 FineMin(X * GTerrainTileSamples * 2, Y * GTerrainTileSamples * 2);

        TVector<float>  FineHeights(kFine * kFine);
        TVector<uint32> FineWeights(kFine * kFine);
        ReadHeights(FinerMip, FineMin, FIntVector2(kFine, kFine), FineHeights.GetData());
        ReadWeights(FinerMip, FineMin, FIntVector2(kFine, kFine), FineWeights.GetData());

        TSharedPtr<FTile> Tile = MakeShared<FTile>();
        Tile->Heights.SetNum(GTerrainTileSampleCount);
        const bool bPainted = !Algo::AllOf(FineWeights, [](uint32 Weight) { return Weight == 0u; });
        if (bPainted)
        {
            Tile->Weights.SetNum(GTerrainTileSampleCount);
        }

        for (int32 TY = 0; TY < GTerrainTileEdge; ++TY)
        {
            for (int32 TX = 0; TX < GTerrainTileEdge; ++TX)
            {
                const int32 Fine = (TY * 2) * kFine + TX * 2;
                Tile->Heights[TY * GTerrainTileEdge + TX] = FineHeights[Fine];
                if (bPainted)
                {
                    const uint32 Quad[4] = { FineWeights[Fine], FineWeights[Fine + 1], FineWeights[Fine + kFine], FineWeights[Fine + kFine + 1] };
                    uint32 Packed = 0u;
                    for (int32 Channel = 0; Channel < 4; ++Channel)
                    {
                        const uint32 Shift = uint32(Channel) * 8u;
                        uint32 Sum = 0u;
                        for (uint32 Sample : Quad)
                        {
                            Sum += (Sample >> Shift) & 0xFFu;
                        }
                        Packed |= ((Sum + 2u) / 4u) << Shift;
                    }
                    Tile->Weights[TY * GTerrainTileEdge + TX] = Packed;
                }
            }
        }
        return Tile;
    }

    // Finest-sample rect, inclusive.
    void FTerrainStore::RefreshFrom(const FIntVector2& Min, const FIntVector2& Max)
    {
        ++EditSerial;
        for (int32 Mip = 1; Mip <= PageDepth; ++Mip)
        {
            const int32 Depth   = PageDepth - Mip;
            const int32 Side    = 1 << Depth;
            const int32 MipLast = GetMipLast(Mip);
            const FIntVector2 TilesX = TilesTouching(Min.x >> Mip, Math::Min((Max.x >> Mip) + 1, MipLast), Side);
            const FIntVector2 TilesY = TilesTouching(Min.y >> Mip, Math::Min((Max.y >> Mip) + 1, MipLast), Side);
            for (int32 TY = TilesY.x; TY <= TilesY.y; ++TY)
            {
                for (int32 TX = TilesX.x; TX <= TilesX.y; ++TX)
                {
                    if (HasTileData(Depth, TX, TY))
                    {
                        PublishTile(NodeIndex(Depth, TX, TY), BuildCoarserTile(Depth, TX, TY), true);
                    }
                }
            }
        }
        RefreshBounds(Min, Max);
    }

    void FTerrainStore::NoteChanged(uint32 Node)
    {
        if (ChangeLog.Num() >= kMaxChangeLog)
        {
            ChangeLog.Reset();
            ChangeLogBase = ChangeSerial;
        }
        ChangeLog.Add(Node);
        ++ChangeSerial;
    }

    bool FTerrainStore::GetChangedNodes(uint64 Serial, TVector<uint32>& OutNodes) const
    {
        OutNodes.Reset();
        if (Serial < ChangeLogBase)
        {
            return false;
        }
        for (uint64 Index = Serial - ChangeLogBase; Index < (uint64)ChangeLog.Num(); ++Index)
        {
            OutNodes.Add(ChangeLog[(int32)Index]);
        }
        return true;
    }

    // A finest tile's bounds cover its far edge too, which the next tile starts on, so an edit on a seam reaches one tile back.
    void FTerrainStore::RefreshBounds(const FIntVector2& Min, const FIntVector2& Max)
    {
        const int32 Side = 1 << PageDepth;
        const int32 Last = Resolution - 1;
        const FIntVector2 TilesX = TilesTouching(Min.x, Max.x, Side);
        const FIntVector2 TilesY = TilesTouching(Min.y, Max.y, Side);

        float Samples[GTerrainTileSampleCount];
        for (int32 Y = TilesY.x; Y <= TilesY.y; ++Y)
        {
            for (int32 X = TilesX.x; X <= TilesX.y; ++X)
            {
                FVector2 Range = kEmptyBounds;
                if (!IsTileEmpty(PageDepth, X, Y))
                {
                    // Only samples on the map, since a tile past the far edge holds repeats of it.
                    const FIntVector2 Origin = FIntVector2(X, Y) * GTerrainTileSamples;
                    const FIntVector2 Count(Math::Min(GTerrainTileSamples, Last - Origin.x) + 1, Math::Min(GTerrainTileSamples, Last - Origin.y) + 1);
                    ReadHeights(0, Origin, Count, Samples);
                    for (int32 Index = 0; Index < Count.x * Count.y; ++Index)
                    {
                        Range = Union(Range, FVector2(Samples[Index], Samples[Index]));
                    }
                }
                const uint32 Node = NodeIndex(PageDepth, X, Y);
                if (Bounds[(int32)Node] != Range)
                {
                    Bounds[(int32)Node] = Range;
                    NoteChanged(Node);
                }
            }
        }

        FIntVector2 Lo(TilesX.x, TilesY.x), Hi(TilesX.y, TilesY.y);
        for (int32 Depth = PageDepth - 1; Depth >= 0; --Depth)
        {
            Lo = FIntVector2(Lo.x >> 1, Lo.y >> 1);
            Hi = FIntVector2(Hi.x >> 1, Hi.y >> 1);
            for (int32 Y = Lo.y; Y <= Hi.y; ++Y)
            {
                for (int32 X = Lo.x; X <= Hi.x; ++X)
                {
                    FVector2 Range = kEmptyBounds;
                    for (int32 Child = 0; Child < 4; ++Child)
                    {
                        Range = Union(Range, Bounds[(int32)NodeIndex(Depth + 1, X * 2 + (Child & 1), Y * 2 + (Child >> 1))]);
                    }
                    const uint32 Node = NodeIndex(Depth, X, Y);
                    if (Bounds[(int32)Node] != Range)
                    {
                        Bounds[(int32)Node] = Range;
                        NoteChanged(Node);
                    }
                }
            }
        }
    }

    TSharedPtr<FTerrainStore> FTerrainStore::CreateFromArrays(int32 InResolution, TSpan<const float> Heights, TSpan<const uint8> LayerWeights, int32 LayerCount)
    {
        TSharedPtr<FTerrainStore> Store(new FTerrainStore());
        Store->InitLayout(InResolution);
        const int32 Res  = Store->Resolution;
        const int32 Last = Res - 1;
        const int64 Slice = int64(Res) * Res;
        const bool  bHeights = (int64)Heights.Num() >= Slice;
        int32 Layers = 0;
        while (Layers < (int32)GTerrainMaxLayers && Layers < LayerCount && int64(Layers + 1) * Slice <= (int64)LayerWeights.Num())
        {
            ++Layers;
        }

        const int32 Side = 1 << Store->PageDepth;
        Jobs::ParallelFor((uint32)(Side * Side), [&](uint32 Index)
        {
            const int32 TX = (int32)Index % Side;
            const int32 TY = (int32)Index / Side;
            if (!Store->HasTileData(Store->PageDepth, TX, TY))
            {
                return;
            }
            TSharedPtr<FTile> Tile = MakeShared<FTile>();
            Tile->Heights.SetNum(GTerrainTileSampleCount);
            bool bPainted = false;
            TVector<uint32> Weights(GTerrainTileSampleCount);
            for (int32 Y = 0; Y < GTerrainTileEdge; ++Y)
            {
                const int64 Row = int64(Math::Min(TY * GTerrainTileSamples + Y, Last)) * Res;
                for (int32 X = 0; X < GTerrainTileEdge; ++X)
                {
                    const int64 Sample = Row + Math::Min(TX * GTerrainTileSamples + X, Last);
                    Tile->Heights[Y * GTerrainTileEdge + X] = bHeights ? Math::Clamp(Heights[(int32)Sample], 0.0f, 1.0f) : 0.0f;
                    uint32 Packed = 0u;
                    for (int32 Layer = 0; Layer < Layers; ++Layer)
                    {
                        Packed |= uint32(LayerWeights[(int32)(Layer * Slice + Sample)]) << (Layer * 8);
                    }
                    Weights[Y * GTerrainTileEdge + X] = Packed;
                    bPainted = bPainted || Packed != 0u;
                }
            }
            if (bPainted)
            {
                Tile->Weights = Move(Weights);
            }
            Store->PublishTile(Store->NodeIndex(Store->PageDepth, TX, TY), Tile, true);
        }, 16);

        Store->RefreshFrom(FIntVector2(0), FIntVector2(Last));
        return Store;
    }

    TSharedPtr<FTerrainStore> FTerrainStore::CreateFlat(int32 InResolution, float Height01)
    {
        TVector<float> Heights;
        Heights.Init(Height01, InResolution * InResolution);
        return CreateFromArrays(InResolution, Heights, {}, 0);
    }

    void FTerrainStore::WriteHeights(const FIntVector2& Min, const FIntVector2& Size, const float* In)
    {
        const int32 Last = Resolution - 1;
        const FIntVector2 Lo(Math::Max(Min.x, 0), Math::Max(Min.y, 0));
        const FIntVector2 Hi(Math::Min(Min.x + Size.x - 1, Last), Math::Min(Min.y + Size.y - 1, Last));
        if (Lo.x > Hi.x || Lo.y > Hi.y)
        {
            return;
        }
        const int32 Side = 1 << PageDepth;
        const FIntVector2 TilesX = TilesTouching(Lo.x, Hi.x, Side);
        const FIntVector2 TilesY = TilesTouching(Lo.y, Hi.y, Side);
        for (int32 TY = TilesY.x; TY <= TilesY.y; ++TY)
        {
            for (int32 TX = TilesX.x; TX <= TilesX.y; ++TX)
            {
                const uint32 Node = NodeIndex(PageDepth, TX, TY);
                const FTileRef Old = GetTile(Node);
                TSharedPtr<FTile> Tile = Old != nullptr ? MakeShared<FTile>(*Old) : MakeShared<FTile>();
                Tile->Heights.SetNum(GTerrainTileSampleCount);
                for (int32 Y = Math::Max(Lo.y, TY * GTerrainTileSamples); Y <= Math::Min(Hi.y, TY * GTerrainTileSamples + GTerrainTileSamples); ++Y)
                {
                    for (int32 X = Math::Max(Lo.x, TX * GTerrainTileSamples); X <= Math::Min(Hi.x, TX * GTerrainTileSamples + GTerrainTileSamples); ++X)
                    {
                        Tile->Heights[(Y - TY * GTerrainTileSamples) * GTerrainTileEdge + (X - TX * GTerrainTileSamples)]
                            = Math::Clamp(In[(Y - Min.y) * Size.x + (X - Min.x)], 0.0f, 1.0f);
                    }
                }
                if (bEditing && EditSlot.Find(Node) == nullptr)
                {
                    EditSlot.Add(Node, Edit.Nodes.Num());
                    Edit.Nodes.Add(Node);
                    Edit.Before.Add(Old);
                }
                PublishTile(Node, Tile, true);
            }
        }
        RefreshFrom(Lo, Hi);
    }

    void FTerrainStore::WriteWeights(const FIntVector2& Min, const FIntVector2& Size, const uint32* In)
    {
        const int32 Last = Resolution - 1;
        const FIntVector2 Lo(Math::Max(Min.x, 0), Math::Max(Min.y, 0));
        const FIntVector2 Hi(Math::Min(Min.x + Size.x - 1, Last), Math::Min(Min.y + Size.y - 1, Last));
        if (Lo.x > Hi.x || Lo.y > Hi.y)
        {
            return;
        }
        const int32 Side = 1 << PageDepth;
        const FIntVector2 TilesX = TilesTouching(Lo.x, Hi.x, Side);
        const FIntVector2 TilesY = TilesTouching(Lo.y, Hi.y, Side);
        for (int32 TY = TilesY.x; TY <= TilesY.y; ++TY)
        {
            for (int32 TX = TilesX.x; TX <= TilesX.y; ++TX)
            {
                const uint32 Node = NodeIndex(PageDepth, TX, TY);
                const FTileRef Old = GetTile(Node);
                TSharedPtr<FTile> Tile = Old != nullptr ? MakeShared<FTile>(*Old) : MakeShared<FTile>();
                Tile->Heights.SetNum(GTerrainTileSampleCount);
                if (Tile->Weights.IsEmpty())
                {
                    Tile->Weights.Init(0u, GTerrainTileSampleCount);
                }
                for (int32 Y = Math::Max(Lo.y, TY * GTerrainTileSamples); Y <= Math::Min(Hi.y, TY * GTerrainTileSamples + GTerrainTileSamples); ++Y)
                {
                    for (int32 X = Math::Max(Lo.x, TX * GTerrainTileSamples); X <= Math::Min(Hi.x, TX * GTerrainTileSamples + GTerrainTileSamples); ++X)
                    {
                        Tile->Weights[(Y - TY * GTerrainTileSamples) * GTerrainTileEdge + (X - TX * GTerrainTileSamples)]
                            = In[(Y - Min.y) * Size.x + (X - Min.x)];
                    }
                }
                if (bEditing && EditSlot.Find(Node) == nullptr)
                {
                    EditSlot.Add(Node, Edit.Nodes.Num());
                    Edit.Nodes.Add(Node);
                    Edit.Before.Add(Old);
                }
                PublishTile(Node, Tile, true);
            }
        }
        RefreshFrom(Lo, Hi);
    }

    void FTerrainStore::RemapLayers(const int32 (&Source)[GTerrainMaxLayers])
    {
        const uint32 Offset = GetTerrainNodeDepthOffset(PageDepth);
        const uint32 Side   = 1u << PageDepth;
        TVector<uint32> Painted;
        {
            TScopeLock Guard(CacheLock);
            for (const auto& [Node, Entry] : Cache)
            {
                if (Node >= Offset && Entry.Tile != nullptr && !Entry.Tile->Weights.IsEmpty())
                {
                    Painted.Add(Node);
                }
            }
        }
        {
            FReadScopeLock Guard(FileLock);
            if (const FTileEntry* Entries = Directory())
            {
                for (uint32 Node = Offset; Node < TileCount; ++Node)
                {
                    if (Entries[Node].WeightBytes != 0)
                    {
                        Painted.Add(Node);
                    }
                }
            }
        }
        Algo::Sort(Painted);

        const int32 Last = Resolution - 1;
        for (int32 Index = 0; Index < Painted.Num(); ++Index)
        {
            const uint32 Node = Painted[Index];
            const FTileRef Old = Index > 0 && Painted[Index - 1] == Node ? nullptr : GetTile(Node);
            if (Old == nullptr || Old->Weights.IsEmpty())
            {
                continue;
            }
            TSharedPtr<FTile> Tile = MakeShared<FTile>(*Old);
            for (uint32& Packed : Tile->Weights)
            {
                uint32 Remapped = 0u;
                for (int32 Channel = 0; Channel < (int32)GTerrainMaxLayers; ++Channel)
                {
                    if (Source[Channel] >= 0)
                    {
                        Remapped |= ((Packed >> (Source[Channel] * 8)) & 0xFFu) << (Channel * 8);
                    }
                }
                Packed = Remapped;
            }
            if (bEditing && EditSlot.Find(Node) == nullptr)
            {
                EditSlot.Add(Node, Edit.Nodes.Num());
                Edit.Nodes.Add(Node);
                Edit.Before.Add(Old);
            }
            PublishTile(Node, Tile, true);
            const int32 TX = int32((Node - Offset) % Side) * GTerrainTileSamples;
            const int32 TY = int32((Node - Offset) / Side) * GTerrainTileSamples;
            RefreshFrom(FIntVector2(TX, TY), FIntVector2(Math::Min(TX + GTerrainTileSamples, Last), Math::Min(TY + GTerrainTileSamples, Last)));
        }
    }

    void FTerrainStore::BeginEdit()
    {
        bEditing = true;
        Edit = {};
        EditSlot.Reset();
    }

    FTerrainStore::FEdit FTerrainStore::EndEdit()
    {
        bEditing = false;
        Edit.After.Reset();
        for (uint32 Node : Edit.Nodes)
        {
            Edit.After.Add(GetTile(Node));
        }
        EditSlot.Reset();
        return Move(Edit);
    }

    void FTerrainStore::ApplyEdit(const FTerrainStore::FEdit& InEdit, bool bUndo)
    {
        FIntVector2 Lo(TNumericLimits<int32>::Max()), Hi(TNumericLimits<int32>::Min());
        const uint32 Offset = GetTerrainNodeDepthOffset(PageDepth);
        const uint32 Side   = 1u << PageDepth;
        for (int32 Index = 0; Index < InEdit.Nodes.Num(); ++Index)
        {
            const uint32 Node = InEdit.Nodes[Index];
            PublishTile(Node, bUndo ? InEdit.Before[Index] : InEdit.After[Index], true);
            const FIntVector2 Tile((int32)((Node - Offset) % Side), (int32)((Node - Offset) / Side));
            Lo = FIntVector2(Math::Min(Lo.x, Tile.x * GTerrainTileSamples), Math::Min(Lo.y, Tile.y * GTerrainTileSamples));
            Hi = FIntVector2(Math::Max(Hi.x, Tile.x * GTerrainTileSamples + GTerrainTileSamples), Math::Max(Hi.y, Tile.y * GTerrainTileSamples + GTerrainTileSamples));
        }
        if (Lo.x <= Hi.x)
        {
            const int32 Last = Resolution - 1;
            RefreshFrom(FIntVector2(Math::Min(Lo.x, Last), Math::Min(Lo.y, Last)), FIntVector2(Math::Min(Hi.x, Last), Math::Min(Hi.y, Last)));
        }
    }

    bool FTerrainStore::IsDirty() const
    {
        TScopeLock Guard(CacheLock);
        for (const auto& [Node, Entry] : Cache)
        {
            if (Entry.bDirty)
            {
                return true;
            }
        }
        return false;
    }

    void FTerrainStore::EncodeTile(const FTile& Tile, TVector<uint8>& OutHeights, TVector<uint8>& OutWeights) const
    {
        uint16 Deltas[GTerrainTileSampleCount];
        DeltaEncode(Tile.Heights.GetData(), Deltas);
        Compress(Deltas, sizeof(Deltas), OutHeights);
        OutWeights.Reset();
        if (!Tile.Weights.IsEmpty() && !Algo::AllOf(Tile.Weights, [](uint32 Weight) { return Weight == 0u; }))
        {
            Compress(Tile.Weights.GetData(), Tile.Weights.Num() * sizeof(uint32), OutWeights);
        }
    }

    bool FTerrainStore::MapFile(FStringView Path)
    {
        TScopeLock Guard(FileLock);
        File = Filesystem::FMappedFile::OpenRead(Path);
        if (!File.IsValid() || File.Size() < (int64)sizeof(FStoreHeader))
        {
            File.Close();
            return false;
        }
        const FStoreHeader* Header = reinterpret_cast<const FStoreHeader*>(File.Data());
        if (Header->Magic != kStoreMagic || Header->Version != kStoreVersion || Header->TileCount != GetTerrainNodeCount(GetTerrainPageDepth(GetTerrainTreeDepth(Header->Resolution))))
        {
            LOG_ERROR("Terrain store {} is not a version {} terrain file.", Path, kStoreVersion);
            File.Close();
            return false;
        }
        DiskPath = FString(Path);
        return true;
    }

    TSharedPtr<FTerrainStore> FTerrainStore::Open(FStringView Path)
    {
        TSharedPtr<FTerrainStore> Store(new FTerrainStore());
        if (!Store->MapFile(Path))
        {
            return nullptr;
        }
        const FStoreHeader* Header = reinterpret_cast<const FStoreHeader*>(Store->File.Data());
        Store->InitLayout(Header->Resolution);
        Memory::Memcpy(Store->Bounds.GetData(), Store->File.Data() + Header->BoundsOffset, size_t(Store->TileCount) * sizeof(FVector2));
        return Store;
    }

    // Writes every tile, re-encoding those in memory and copying the rest compressed from the old file.
    bool FTerrainStore::WriteFile(FStringView Path)
    {
        const FPathString TempPath = FPathString(Path) + ".tmp";
        Filesystem::MakeParentDirectoryTree(Path);
        {
            Filesystem::FFileHandle Out = Filesystem::Open(TempPath, Filesystem::EAccess::Write, Filesystem::ECreateMode::CreateAlways,
                                                           Filesystem::EShare::None, Filesystem::EHint::Sequential);
            if (!Out.IsValid())
            {
                LOG_ERROR("Terrain store could not write {}.", TempPath);
                return false;
            }

            FStoreHeader Header;
            Header.Resolution      = Resolution;
            Header.PageDepth       = PageDepth;
            Header.TileCount       = TileCount;
            Header.BoundsOffset    = sizeof(FStoreHeader);
            Header.DirectoryOffset = Header.BoundsOffset + uint64(TileCount) * sizeof(FVector2);
            Header.DataOffset      = Header.DirectoryOffset + uint64(TileCount) * sizeof(FTileEntry);

            TVector64<FTileEntry> Entries;
            Entries.SetNum(TileCount);
            uint64 Cursor = Header.DataOffset;
            Out.Seek((int64)Cursor);
            const FTileEntry* OldEntries = Directory();

            TVector<TVector<uint8>> Heights(kSaveBatch), Weights(kSaveBatch);
            TVector<FTileRef> Tiles(kSaveBatch);
            for (uint32 First = 0; First < TileCount; First += kSaveBatch)
            {
                const uint32 Count = Math::Min(kSaveBatch, TileCount - First);
                for (uint32 Index = 0; Index < Count; ++Index)
                {
                    TScopeLock Guard(CacheLock);
                    const FCachedTile* Cached = Cache.Find(First + Index);
                    Tiles[(int32)Index] = Cached != nullptr ? Cached->Tile : nullptr;
                }
                Jobs::ParallelFor(Count, [&](uint32 Index)
                {
                    Heights[(int32)Index].Reset();
                    Weights[(int32)Index].Reset();
                    if (Tiles[(int32)Index] != nullptr)
                    {
                        EncodeTile(*Tiles[(int32)Index], Heights[(int32)Index], Weights[(int32)Index]);
                    }
                }, 32);

                FReadScopeLock FileGuard(FileLock);
                for (uint32 Index = 0; Index < Count; ++Index)
                {
                    FTileEntry& Entry = Entries[First + Index];
                    if (Tiles[(int32)Index] != nullptr)
                    {
                        Entry = { Cursor, (uint32)Heights[(int32)Index].Num(), (uint32)Weights[(int32)Index].Num() };
                        Out.Write(Heights[(int32)Index].GetData(), Heights[(int32)Index].Num());
                        Out.Write(Weights[(int32)Index].GetData(), Weights[(int32)Index].Num());
                    }
                    else if (OldEntries != nullptr && OldEntries[First + Index].HeightBytes != 0)
                    {
                        const FTileEntry& Old = OldEntries[First + Index];
                        Entry = { Cursor, Old.HeightBytes, Old.WeightBytes };
                        Out.Write(File.Data() + Old.Offset, int64(Old.HeightBytes) + Old.WeightBytes);
                    }
                    Cursor += uint64(Entry.HeightBytes) + Entry.WeightBytes;
                }
            }

            Out.WriteAt(&Header, sizeof(Header), 0);
            Out.WriteAt(Bounds.GetData(), int64(TileCount) * sizeof(FVector2), (int64)Header.BoundsOffset);
            Out.WriteAt(Entries.GetData(), int64(TileCount) * sizeof(FTileEntry), (int64)Header.DirectoryOffset);
            Out.Flush();
        }

        {
            TScopeLock Guard(FileLock);
            File.Close();
        }
        if (!Filesystem::Move(TempPath, Path, true) || !MapFile(Path))
        {
            LOG_ERROR("Terrain store could not replace {}.", Path);
            return false;
        }
        TScopeLock Guard(CacheLock);
        for (auto& [Node, Entry] : Cache)
        {
            Entry.bDirty = false;
        }
        return true;
    }

    // Edited tiles go on the end and their directory entries are rewritten in place, so a save costs only what changed.
    bool FTerrainStore::AppendDirty()
    {
        TVector<uint32>   Nodes;
        TVector<FTileRef> Tiles;
        {
            TScopeLock Guard(CacheLock);
            for (const auto& [Node, Entry] : Cache)
            {
                if (Entry.bDirty)
                {
                    Nodes.Add(Node);
                    Tiles.Add(Entry.Tile);
                }
            }
        }

        TVector<TVector<uint8>> Heights(Nodes.Num()), Weights(Nodes.Num());
        Jobs::ParallelFor((uint32)Nodes.Num(), [&](uint32 Index)
        {
            if (Tiles[(int32)Index] != nullptr)
            {
                EncodeTile(*Tiles[(int32)Index], Heights[(int32)Index], Weights[(int32)Index]);
            }
        }, 32);

        const FString Path = DiskPath;
        uint64 DirectoryOffset = 0;
        uint64 BoundsOffset    = 0;
        {
            TScopeLock Guard(FileLock);
            const FStoreHeader* Header = reinterpret_cast<const FStoreHeader*>(File.Data());
            DirectoryOffset = Header->DirectoryOffset;
            BoundsOffset    = Header->BoundsOffset;
            File.Close();
        }

        {
            Filesystem::FFileHandle Out = Filesystem::Open(Path, Filesystem::EAccess::ReadWrite, Filesystem::ECreateMode::OpenExisting, Filesystem::EShare::None);
            if (!Out.IsValid())
            {
                LOG_ERROR("Terrain store could not open {} to save edits.", Path);
                MapFile(Path);
                return false;
            }
            uint64 Cursor = (uint64)Out.Size();
            for (int32 Index = 0; Index < Nodes.Num(); ++Index)
            {
                const FTileEntry Entry = { Cursor, (uint32)Heights[Index].Num(), (uint32)Weights[Index].Num() };
                Out.WriteAt(Heights[Index].GetData(), Heights[Index].Num(), (int64)Cursor);
                Out.WriteAt(Weights[Index].GetData(), Weights[Index].Num(), (int64)(Cursor + Entry.HeightBytes));
                Out.WriteAt(&Entry, sizeof(Entry), (int64)(DirectoryOffset + uint64(Nodes[Index]) * sizeof(FTileEntry)));
                Cursor += uint64(Entry.HeightBytes) + Entry.WeightBytes;
            }
            Out.WriteAt(Bounds.GetData(), int64(TileCount) * sizeof(FVector2), (int64)BoundsOffset);
            Out.Flush();
        }

        if (!MapFile(Path))
        {
            return false;
        }
        TScopeLock Guard(CacheLock);
        for (uint32 Node : Nodes)
        {
            if (FCachedTile* Entry = Cache.Find(Node))
            {
                Entry->bDirty = false;
            }
        }
        return true;
    }

    bool FTerrainStore::Save(FStringView Path)
    {
        if (File.IsValid() && DiskPath == Path)
        {
            return !IsDirty() || AppendDirty();
        }
        return WriteFile(Path);
    }

    // One pass over the rows feeds every mip at once, since a coarse mip's row is a decimated finest row.
    TSharedPtr<FTerrainStore> FTerrainStore::Import(FStringView Path, int32 InResolution, const FRowSource& Rows)
    {
        TSharedPtr<FTerrainStore> Store(new FTerrainStore());
        Store->InitLayout(InResolution);
        const int32 Res       = Store->Resolution;
        const int32 Last      = Res - 1;
        const int32 PageDepth = Store->PageDepth;
        const uint32 TileCount = Store->TileCount;

        const FPathString TempPath = FPathString(Path) + ".tmp";
        Filesystem::MakeParentDirectoryTree(Path);
        Filesystem::FFileHandle Out = Filesystem::Open(TempPath, Filesystem::EAccess::Write, Filesystem::ECreateMode::CreateAlways,
                                                       Filesystem::EShare::None, Filesystem::EHint::Sequential);
        if (!Out.IsValid())
        {
            LOG_ERROR("Terrain import could not write {}.", TempPath);
            return nullptr;
        }

        FStoreHeader Header;
        Header.Resolution      = Res;
        Header.PageDepth       = PageDepth;
        Header.TileCount       = TileCount;
        Header.BoundsOffset    = sizeof(FStoreHeader);
        Header.DirectoryOffset = Header.BoundsOffset + uint64(TileCount) * sizeof(FVector2);
        Header.DataOffset      = Header.DirectoryOffset + uint64(TileCount) * sizeof(FTileEntry);
        TVector64<FTileEntry> Entries;
        Entries.SetNum(TileCount);
        uint64 Cursor = Header.DataOffset;
        Out.Seek((int64)Cursor);

        // Each mip collects one row of tiles, 65 rows since neighboring tile rows share their seam row.
        struct FMipRows
        {
            int32          Side  = 0;
            int32          Width = 0;
            TVector<float> Samples;
            TVector<float> Scratch;
        };
        TVector<FMipRows> Mips(PageDepth + 1);
        for (int32 Mip = 0; Mip <= PageDepth; ++Mip)
        {
            Mips[Mip].Side  = 1 << (PageDepth - Mip);
            Mips[Mip].Width = Mips[Mip].Side * GTerrainTileSamples + 1;
            Mips[Mip].Samples.SetNum(Mips[Mip].Width * GTerrainTileEdge);
            Mips[Mip].Scratch.SetNum(Mips[Mip].Width);
        }

        TVector<TVector<uint8>> Encoded;
        auto FlushTileRow = [&](int32 Mip, int32 TileRow, int32 FilledRows)
        {
            FMipRows& Level = Mips[Mip];
            if (TileRow >= Level.Side)
            {
                return;
            }
            const int32 Width = Level.Width;
            // Rows past the map repeat the last one, as reads clamp there.
            for (int32 Row = FilledRows; Row < GTerrainTileEdge; ++Row)
            {
                Memory::Memcpy(&Level.Samples[Row * Width], &Level.Samples[(FilledRows - 1) * Width], size_t(Width) * sizeof(float));
            }
            const int32 Depth = PageDepth - Mip;
            if (Mip == 0)
            {
                for (int32 TX = 0; TX < Level.Side; ++TX)
                {
                    if (Store->IsTileEmpty(Depth, TX, TileRow))
                    {
                        continue;
                    }
                    FVector2 Range = kEmptyBounds;
                    const int32 Columns = Math::Min(GTerrainTileSamples, Last - TX * GTerrainTileSamples) + 1;
                    for (int32 Y = 0; Y < FilledRows; ++Y)
                    {
                        const float* Row = &Level.Samples[Y * Width + TX * GTerrainTileSamples];
                        for (int32 X = 0; X < Columns; ++X)
                        {
                            Range = Union(Range, FVector2(Row[X], Row[X]));
                        }
                    }
                    Store->Bounds[(int32)Store->NodeIndex(Depth, TX, TileRow)] = Range;
                }
            }
            Encoded.SetNum(Level.Side);
            Jobs::ParallelFor((uint32)Level.Side, [&](uint32 TX)
            {
                Encoded[(int32)TX].Reset();
                if (!Store->HasTileData(Depth, (int32)TX, TileRow))
                {
                    return;
                }
                FTile Tile;
                Tile.Heights.SetNum(GTerrainTileSampleCount);
                for (int32 Y = 0; Y < GTerrainTileEdge; ++Y)
                {
                    Memory::Memcpy(&Tile.Heights[Y * GTerrainTileEdge], &Level.Samples[Y * Width + (int32)TX * GTerrainTileSamples],
                                   GTerrainTileEdge * sizeof(float));
                }
                TVector<uint8> NoWeights;
                Store->EncodeTile(Tile, Encoded[(int32)TX], NoWeights);
            }, 8);
            for (int32 TX = 0; TX < Level.Side; ++TX)
            {
                if (Encoded[TX].IsEmpty())
                {
                    continue;
                }
                Entries[Store->NodeIndex(Depth, TX, TileRow)] = { Cursor, (uint32)Encoded[TX].Num(), 0u };
                Out.Write(Encoded[TX].GetData(), Encoded[TX].Num());
                Cursor += (uint64)Encoded[TX].Num();
            }
        };

        // A row on a seam closes the tile row above as its far edge, then opens the next.
        auto AddMipRow = [&](int32 Mip, int32 MipRow, const float* Source)
        {
            FMipRows& Level = Mips[Mip];
            const int32 Width = Level.Width;
            for (int32 X = 0; X < Width; ++X)
            {
                Level.Scratch[X] = Math::Clamp(Source[Math::Min(X << Mip, Last)], 0.0f, 1.0f);
            }
            const int32 Local   = MipRow & (GTerrainTileSamples - 1);
            const int32 TileRow = MipRow >> GTerrainTileShift;
            if (Local == 0 && MipRow > 0)
            {
                Memory::Memcpy(&Level.Samples[GTerrainTileSamples * Width], Level.Scratch.GetData(), size_t(Width) * sizeof(float));
                FlushTileRow(Mip, TileRow - 1, GTerrainTileEdge);
            }
            Memory::Memcpy(&Level.Samples[Local * Width], Level.Scratch.GetData(), size_t(Width) * sizeof(float));
            if (MipRow == Store->GetMipLast(Mip))
            {
                FlushTileRow(Mip, TileRow, Local + 1);
            }
        };

        TVector<float> Row(Res);
        for (int32 Y = 0; Y <= Last; ++Y)
        {
            Rows(Y, Row.GetData());
            for (int32 Mip = 0; Mip <= PageDepth; ++Mip)
            {
                if ((Y & ((1 << Mip) - 1)) == 0)
                {
                    AddMipRow(Mip, Y >> Mip, Row.GetData());
                }
                else if (Y == Last)
                {
                    // The last finest row is off this mip's grid, and the grid's clamped last row reads it.
                    AddMipRow(Mip, Store->GetMipLast(Mip), Row.GetData());
                }
            }
        }

        for (int32 Depth = PageDepth - 1; Depth >= 0; --Depth)
        {
            const int32 Side = 1 << Depth;
            for (int32 Y = 0; Y < Side; ++Y)
            {
                for (int32 X = 0; X < Side; ++X)
                {
                    FVector2 Range = kEmptyBounds;
                    for (int32 Child = 0; Child < 4; ++Child)
                    {
                        Range = Union(Range, Store->Bounds[(int32)Store->NodeIndex(Depth + 1, X * 2 + (Child & 1), Y * 2 + (Child >> 1))]);
                    }
                    Store->Bounds[(int32)Store->NodeIndex(Depth, X, Y)] = Range;
                }
            }
        }

        Out.WriteAt(&Header, sizeof(Header), 0);
        Out.WriteAt(Store->Bounds.GetData(), int64(TileCount) * sizeof(FVector2), (int64)Header.BoundsOffset);
        Out.WriteAt(Entries.GetData(), int64(TileCount) * sizeof(FTileEntry), (int64)Header.DirectoryOffset);
        Out.Flush();
        Out.Close();

        if (!Filesystem::Move(TempPath, Path, true) || !Store->MapFile(Path))
        {
            LOG_ERROR("Terrain import could not finish {}.", Path);
            return nullptr;
        }
        return Store;
    }

    TSharedPtr<FTerrainStore> FTerrainStore::Resample(const FTerrainStore& Source, int32 NewResolution, FStringView DiskPath)
    {
        const int32 OldRes  = Source.GetResolution();
        const int32 NewRes  = Math::Max(NewResolution, 2);
        const float Scale   = float(OldRes - 1) / float(NewRes - 1);
        TVector<float> Pair(OldRes * 2);
        auto Rows = [&](int32 Row, float* Out)
        {
            const float FY = float(Row) * Scale;
            const int32 Y0 = Math::Min((int32)FY, OldRes - 1);
            Source.ReadHeights(0, FIntVector2(0, Y0), FIntVector2(OldRes, 2), Pair.GetData());
            const float TY = FY - float(Y0);
            for (int32 X = 0; X < NewRes; ++X)
            {
                const float FX = float(X) * Scale;
                const int32 X0 = Math::Min((int32)FX, OldRes - 1);
                const int32 X1 = Math::Min(X0 + 1, OldRes - 1);
                const float TX = FX - float(X0);
                const float Top    = Math::Mix(Pair[X0], Pair[X1], TX);
                const float Bottom = Math::Mix(Pair[OldRes + X0], Pair[OldRes + X1], TX);
                Out[X] = Math::Mix(Top, Bottom, TY);
            }
        };
        if (!DiskPath.IsEmpty())
        {
            return Import(DiskPath, NewRes, Rows);
        }

        const int64 Slice = int64(NewRes) * NewRes;
        TVector<float> Heights((int32)Slice);
        TVector<uint8> Weights((int32)(Slice * GTerrainMaxLayers));
        uint32 Packed = 0u;
        for (int32 Y = 0; Y < NewRes; ++Y)
        {
            Rows(Y, &Heights[Y * NewRes]);
            for (int32 X = 0; X < NewRes; ++X)
            {
                Source.ReadWeights(0, FIntVector2((int32)(float(X) * Scale + 0.5f), (int32)(float(Y) * Scale + 0.5f)), FIntVector2(1, 1), &Packed);
                for (int32 Layer = 0; Layer < (int32)GTerrainMaxLayers; ++Layer)
                {
                    Weights[(int32)(Layer * Slice) + Y * NewRes + X] = uint8((Packed >> (Layer * 8)) & 0xFFu);
                }
            }
        }
        return CreateFromArrays(NewRes, Heights, Weights, (int32)GTerrainMaxLayers);
    }
}
