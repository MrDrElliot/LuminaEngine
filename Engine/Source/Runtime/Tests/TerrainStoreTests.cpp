#include <gtest/gtest.h>

#include "Core/Math/Math.h"
#include "Core/Templates/NumericLimits.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "World/Terrain/TerrainStore.h"
#include "World/Terrain/TerrainStoreRegistry.h"
#include "zstd.h"

#include <cmath>

using namespace Lumina;

namespace
{
    TVector<float> MakeHeights(int32 Resolution)
    {
        TVector<float> Heights(Resolution * Resolution);
        for (int32 Y = 0; Y < Resolution; ++Y)
        {
            for (int32 X = 0; X < Resolution; ++X)
            {
                // Uncorrelated in x and y so a transposed or off-by-one read cannot pass.
                Heights[Y * Resolution + X] = 0.5f + 0.25f * std::sin((float)X * 0.31f) * std::cos((float)Y * 0.17f) + (float)((X * 7 + Y * 13) % 11) * 0.01f;
            }
        }
        return Heights;
    }

    float Reference(const TVector<float>& Heights, int32 Resolution, int32 Mip, int32 GX, int32 GY)
    {
        const int32 Last = Resolution - 1;
        return Heights[Math::Min(GY << Mip, Last) * Resolution + Math::Min(GX << Mip, Last)];
    }

    void ExpectMatchesArrays(const FTerrainStore& Store, const TVector<float>& Heights, float Tolerance)
    {
        const int32 Res = Store.GetResolution();
        for (int32 Mip = 0; Mip <= Store.GetPageDepth(); ++Mip)
        {
            const int32 Side = Store.GetMipLast(Mip) + 1;
            TVector<float> Read(Side * Side);
            Store.ReadHeights(Mip, FIntVector2(0), FIntVector2(Side), Read.GetData());
            for (int32 Y = 0; Y < Side; ++Y)
            {
                for (int32 X = 0; X < Side; ++X)
                {
                    ASSERT_NEAR(Read[Y * Side + X], Reference(Heights, Res, Mip, X, Y), Tolerance) << "mip " << Mip << " at " << X << "," << Y;
                }
            }
        }
    }

    // Every page node covers its full-resolution span including the far edge, clamped to the map.
    void ExpectBoundsMatchArrays(const FTerrainStore& Store, const TVector<float>& Heights, float Tolerance)
    {
        const int32 Res  = Store.GetResolution();
        const int32 Last = Res - 1;
        for (int32 Depth = 0; Depth <= Store.GetPageDepth(); ++Depth)
        {
            const int32 Span = GTerrainTileSamples << (Store.GetPageDepth() - Depth);
            const int32 Side = 1 << Depth;
            for (int32 Y = 0; Y < Side; ++Y)
            {
                for (int32 X = 0; X < Side; ++X)
                {
                    const FVector2 Got = Store.GetBounds()[(int32)(GetTerrainNodeDepthOffset(Depth) + uint32(Y * Side + X))];
                    if (X * Span >= Last || Y * Span >= Last)
                    {
                        EXPECT_GT(Got.x, Got.y) << "empty node " << Depth << " " << X << "," << Y;
                        continue;
                    }
                    float Lo = TNumericLimits<float>::Infinity(), Hi = -TNumericLimits<float>::Infinity();
                    for (int32 SY = Y * Span; SY <= Math::Min(Y * Span + Span, Last); ++SY)
                    {
                        for (int32 SX = X * Span; SX <= Math::Min(X * Span + Span, Last); ++SX)
                        {
                            Lo = Math::Min(Lo, Heights[SY * Res + SX]);
                            Hi = Math::Max(Hi, Heights[SY * Res + SX]);
                        }
                    }
                    EXPECT_NEAR(Got.x, Lo, Tolerance) << "depth " << Depth << " " << X << "," << Y;
                    EXPECT_NEAR(Got.y, Hi, Tolerance) << "depth " << Depth << " " << X << "," << Y;
                }
            }
        }
    }

    FString TempStorePath(const char* Name)
    {
        return Filesystem::GetTempDirectory() + "/LuminaTerrainStoreTests_" + Name + ".lterrain";
    }

    // Mirrors FTerrainStore's private on-disk layout, so a test can corrupt one tile's weight payload in place.
    struct FOnDiskHeader
    {
        uint32 Magic, Version;
        int32  Resolution, PageDepth;
        uint32 TileCount, _Pad;
        uint64 BoundsOffset, DirectoryOffset, DataOffset;
    };
    static_assert(sizeof(FOnDiskHeader) == 48);

    struct FOnDiskTileEntry
    {
        uint64 Offset;
        uint32 HeightBytes;
        uint32 WeightBytes;
    };
}

TEST(TerrainStore, ArraysReadBackAtEveryMip)
{
    for (int32 Res : { 9, 65, 129, 200, 513 })
    {
        const TVector<float> Heights = MakeHeights(Res);
        TSharedPtr<FTerrainStore> Store = FTerrainStore::CreateFromArrays(Res, Heights, {}, 0);
        ExpectMatchesArrays(*Store, Heights, 1e-6f);
        ExpectBoundsMatchArrays(*Store, Heights, 1e-6f);
    }
}

TEST(TerrainStore, CoarseWeightsAverageTheirFootprint)
{
    constexpr int32 Res = 257;
    TVector<uint8> Weights(Res * Res);
    for (int32 Y = 0; Y < Res; ++Y)
    {
        Weights[Y * Res + 1] = 255u;
    }
    TSharedPtr<FTerrainStore> Store = FTerrainStore::CreateFromArrays(Res, MakeHeights(Res), Weights, 1);
    uint32 Mip1 = 0u, Mip2 = 0u;
    Store->ReadWeights(1, FIntVector2(0, 4), FIntVector2(1, 1), &Mip1);
    Store->ReadWeights(2, FIntVector2(0, 4), FIntVector2(1, 1), &Mip2);
    EXPECT_NEAR(float(Mip1 & 0xFFu), 127.5f, 1.0f);
    EXPECT_NEAR(float(Mip2 & 0xFFu), 63.75f, 1.0f);
}

TEST(TerrainStore, WritesMatchARebuildAndUndo)
{
    constexpr int32 Res = 257;
    TVector<float> Heights = MakeHeights(Res);
    TSharedPtr<FTerrainStore> Store = FTerrainStore::CreateFromArrays(Res, Heights, {}, 0);
    const TVector<float> Original = Heights;

    // A rect over a tile seam and the map's far edge.
    const FIntVector2 Min(60, 200), Size(10, 57);
    TVector<float> Patch(Size.x * Size.y, 0.9f);
    Store->BeginEdit();
    Store->WriteHeights(Min, Size, Patch.GetData());
    const FTerrainStore::FEdit Edit = Store->EndEdit();
    for (int32 Y = 0; Y < Size.y; ++Y)
    {
        for (int32 X = 0; X < Size.x; ++X)
        {
            Heights[(Min.y + Y) * Res + Min.x + X] = 0.9f;
        }
    }
    ExpectMatchesArrays(*Store, Heights, 1e-6f);
    ExpectBoundsMatchArrays(*Store, Heights, 1e-6f);

    Store->ApplyEdit(Edit, true);
    ExpectMatchesArrays(*Store, Original, 1e-6f);
    ExpectBoundsMatchArrays(*Store, Original, 1e-6f);
    Store->ApplyEdit(Edit, false);
    ExpectMatchesArrays(*Store, Heights, 1e-6f);
}

TEST(TerrainStore, SaveOpenAndAppendRoundTrip)
{
    constexpr int32 Res = 200;
    TVector<float> Heights = MakeHeights(Res);
    const FString Path = TempStorePath("RoundTrip");
    {
        TSharedPtr<FTerrainStore> Store = FTerrainStore::CreateFromArrays(Res, Heights, {}, 0);
        ASSERT_TRUE(Store->Save(Path));
        EXPECT_FALSE(Store->IsDirty());
    }
    constexpr float kQuantum = 1.0f / 65535.0f;
    {
        TSharedPtr<FTerrainStore> Store = FTerrainStore::Open(Path);
        ASSERT_NE(Store, nullptr);
        ExpectMatchesArrays(*Store, Heights, kQuantum);
        ExpectBoundsMatchArrays(*Store, Heights, kQuantum);

        const float Raised = 0.75f;
        Store->WriteHeights(FIntVector2(70, 70), FIntVector2(1, 1), &Raised);
        Heights[70 * Res + 70] = Raised;
        ASSERT_TRUE(Store->Save(Path));
    }
    TSharedPtr<FTerrainStore> Reopened = FTerrainStore::Open(Path);
    ASSERT_NE(Reopened, nullptr);
    ExpectMatchesArrays(*Reopened, Heights, kQuantum);
    ExpectBoundsMatchArrays(*Reopened, Heights, kQuantum);
    Reopened = nullptr;
    Filesystem::RemoveFile(Path);
}

TEST(TerrainStore, StreamedImportMatchesArrays)
{
    for (int32 Res : { 65, 200, 1025 })
    {
        const TVector<float> Heights = MakeHeights(Res);
        const FString Path = TempStorePath("Import");
        TSharedPtr<FTerrainStore> Store = FTerrainStore::Import(Path, Res, [&](int32 Row, float* Out)
        {
            Memory::Memcpy(Out, &Heights[Row * Res], size_t(Res) * sizeof(float));
        });
        ASSERT_NE(Store, nullptr);
        constexpr float kQuantum = 1.0f / 65535.0f;
        ExpectMatchesArrays(*Store, Heights, kQuantum);
        // Bounds are taken before quantizing, so they hold the exact heights.
        ExpectBoundsMatchArrays(*Store, Heights, 1e-6f);
        Store = nullptr;
        Filesystem::RemoveFile(Path);
    }
}

TEST(TerrainStore, RegistryFreesStoresTheDiskCanGiveBack)
{
    constexpr int32 Res = 129;
    constexpr int32 kCollectCalls = 64;
    const TVector<float> Heights = MakeHeights(Res);
    const FString SavedPath = TempStorePath("RegistrySaved");
    {
        FTerrainStoreRegistry Registry;

        // On disk and unedited, so nothing is lost when the last component lets go.
        TWeakPtr<FTerrainStore> Saved;
        {
            TSharedPtr<FTerrainStore> Store = FTerrainStore::CreateFromArrays(Res, Heights, {}, 0);
            ASSERT_TRUE(Store->Save(SavedPath));
            Registry.Register("/Game/Saved.lterrain", Store);
            Saved = Store;
            for (int32 Call = 0; Call < kCollectCalls; ++Call)
            {
                Registry.Collect();
            }
            EXPECT_FALSE(Saved.IsExpired()) << "a store a component still holds was freed";
        }
        for (int32 Call = 0; Call < kCollectCalls; ++Call)
        {
            Registry.Collect();
        }
        EXPECT_TRUE(Saved.IsExpired());

        // Edited after its save, so undoing the terrain's delete still needs the edit.
        TWeakPtr<FTerrainStore> Edited;
        {
            TSharedPtr<FTerrainStore> Store = FTerrainStore::Open(SavedPath);
            ASSERT_NE(Store, nullptr);
            const float Raised = 0.9f;
            Store->WriteHeights(FIntVector2(10, 10), FIntVector2(1, 1), &Raised);
            Registry.Register("/Game/Edited.lterrain", Store);
            Edited = Store;
        }
        for (int32 Call = 0; Call < kCollectCalls; ++Call)
        {
            Registry.Collect();
        }
        ASSERT_FALSE(Edited.IsExpired());
        EXPECT_NEAR(Edited.Pin()->GetHeight(10, 10), 0.9f, 1e-6f);
    }
    Filesystem::RemoveFile(SavedPath);
}

// A decompress that stops short with no error must be treated as corrupt, not kept with whatever follows it in memory.
TEST(TerrainStore, ShortWeightDecompressIsRejectedNotKeptAsGarbage)
{
    constexpr int32 Res = 65;
    const TVector<uint8> Weights(Res * Res, 255u);
    const FString Path = TempStorePath("ShortWeight");
    {
        TSharedPtr<FTerrainStore> Store = FTerrainStore::CreateFromArrays(Res, MakeHeights(Res), Weights, 1);
        ASSERT_TRUE(Store->Save(Path));
    }

    TVector<uint8> Bytes;
    ASSERT_TRUE(Filesystem::ReadFile(Bytes, Path));
    const auto* Header = reinterpret_cast<const FOnDiskHeader*>(Bytes.GetData());
    FOnDiskTileEntry& Entry = reinterpret_cast<FOnDiskTileEntry*>(Bytes.GetData() + Header->DirectoryOffset)[0];
    ASSERT_GT(Entry.WeightBytes, 0u) << "the root tile must hold weights to corrupt";

    // Half the samples, which decompresses without error yet leaves the destination buffer under-filled.
    const TVector<uint32> Short(GTerrainTileSampleCount / 2, 0xABCDEF01u);
    TVector<uint8> ShortCompressed((int32)ZSTD_compressBound(Short.Num() * sizeof(uint32)));
    const size_t ShortBytes = ZSTD_compress(ShortCompressed.GetData(), (size_t)ShortCompressed.Num(), Short.GetData(), Short.Num() * sizeof(uint32), 1);
    ASSERT_FALSE(ZSTD_isError(ShortBytes));
    ASSERT_LE(ShortBytes, (size_t)Entry.WeightBytes) << "the corrupt payload must fit the space the intact one left";

    Memory::Memcpy(Bytes.GetData() + Entry.Offset + Entry.HeightBytes, ShortCompressed.GetData(), ShortBytes);
    Entry.WeightBytes = (uint32)ShortBytes;
    ASSERT_TRUE(Filesystem::WriteFile(Path, Bytes));

    TSharedPtr<FTerrainStore> Reopened = FTerrainStore::Open(Path);
    ASSERT_NE(Reopened, nullptr);
    uint32 Weight = 0xFFFFFFFFu;
    Reopened->ReadWeights(0, FIntVector2(0, 0), FIntVector2(1, 1), &Weight);
    EXPECT_EQ(Weight, 0u) << "a short weight decompress must read back as unpainted, not garbage";
    Reopened = nullptr;
    Filesystem::RemoveFile(Path);
}
