#include <gtest/gtest.h>

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/Package/PackageNameTable.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"
#include "ScopedTempMountTestUtil.h"
#include "miniz.h"

using namespace Lumina;

namespace
{
    constexpr const char* kCodecAlias = "/PkgCodec";
    constexpr uint32      kChunkMagic = 0x32435A4C;

    void EnsureCodecMount()
    {
        static const bool bReady = []
        {
            RegisterPendingTypes();
            VFS::Mount<VFS::FMemoryFileSystem>(kCodecAlias);
            return true;
        }();
        (void)bReady;
    }

    void AppendU32(TVector<uint8>& Out, uint32 Value)
    {
        const uint8* Bytes = reinterpret_cast<const uint8*>(&Value);
        Out.Append(Bytes, Bytes + sizeof(Value));
    }

    void AppendU64(TVector<uint8>& Out, uint64 Value)
    {
        const uint8* Bytes = reinterpret_cast<const uint8*>(&Value);
        Out.Append(Bytes, Bytes + sizeof(Value));
    }

    uint32 ReadU32At(const TVector<uint8>& Bytes, size_t Offset)
    {
        uint32 Value = 0;
        std::memcpy(&Value, Bytes.GetData() + Offset, sizeof(Value));
        return Value;
    }

    TVector<uint8> MakeCompressiblePayload(size_t Length)
    {
        TVector<uint8> Payload;
        Payload.Reserve(Lumina::IntCastChecked<Lumina::int32>(Length));
        const char* Pattern = "FloatProperty NoiseScale StructProperty ShapeSize ";
        for (size_t i = 0; i < Length; ++i)
        {
            Payload.Add((uint8)Pattern[i % strlen(Pattern)]);
        }
        return Payload;
    }

    // Pseudo-random bytes deflate to about their own length, which is how a test file stays above the whole-file read limit.
    TVector<uint8> MakeIncompressiblePayload(size_t Length)
    {
        TVector<uint8> Payload;
        Payload.Reserve(Lumina::IntCastChecked<Lumina::int32>(Length));
        uint32 State = 0x9E3779B9u;
        for (size_t i = 0; i < Length; ++i)
        {
            State = State * 1664525u + 1013904223u;
            Payload.Add((uint8)(State >> 24));
        }
        return Payload;
    }

    // The v1 layout carried no codec word because deflate was the only one, and it is the simplest container a test can write by hand.
    TVector<uint8> WrapInLegacyDeflateContainer(const TVector<uint8>& Payload)
    {
        mz_ulong Bound = mz_compressBound((mz_ulong)Payload.Num());
        TVector<uint8> Deflated(Lumina::IntCastChecked<Lumina::int32>(Bound));
        mz_ulong DeflatedLength = Bound;
        EXPECT_EQ(mz_compress2(Deflated.GetData(), &DeflatedLength, Payload.GetData(),
                               (mz_ulong)Payload.Num(), MZ_DEFAULT_LEVEL), MZ_OK);
        Deflated.SetNum(Lumina::IntCastChecked<Lumina::int32>(DeflatedLength));

        TVector<uint8> Container;
        AppendU32(Container, kChunkMagic);
        AppendU32(Container, 1);
        AppendU64(Container, Payload.Num());
        AppendU32(Container, uint32(4 * Constants::kMiB));
        AppendU32(Container, 1);
        AppendU32(Container, (uint32)Deflated.Num());
        Container.Append(Deflated);
        return Container;
    }

    bool WriteBytes(FStringView Path, const TVector<uint8>& Bytes)
    {
        return VFS::WriteFile(Path, TSpan<const uint8>(Bytes.GetData(), Bytes.Num()));
    }

    void RetirePackage(CPackage* Package)
    {
        TVector<CObject*> Objects;
        GetObjectsWithPackage(Package, Objects);
        for (CObject* Object : Objects)
        {
            if (Object != Package)
            {
                Object->ConditionalBeginDestroy();
            }
        }
        Package->ExportTable.Reset();
        Package->ImportTable.Reset();

        // The root set holds the package's only strong reference, so unrooting is what destroys it.
        Package->RemoveFromRoot();
    }

    // Saves one emitter at Path and lets the package go, so a later load has to come from the file.
    FGuid SaveEmitterPackage(const char* Path, bool bRegisterAsset = false)
    {
        CParticleEmitter::StaticClass()->Link();
        const FGuid Guid = FGuid::New();

        CPackage* Package = CPackage::CreatePackage(Path);
        EXPECT_NE(Package, nullptr);

        CParticleEmitter* Emitter = Cast<CParticleEmitter>(
            NewObject(CParticleEmitter::StaticClass(), Package, "CodecEmitter", Guid));
        EXPECT_NE(Emitter, nullptr);
        Emitter->NoiseScale = 3.5f;

        EXPECT_TRUE(CPackage::SavePackage(Package, Path));
        if (bRegisterAsset)
        {
            FAssetRegistry::Get().AssetCreated(Emitter);
        }
        RetirePackage(Package);
        return Guid;
    }

    // A refused load leaves a package shell in the failed state, which would answer every later load of the same path without reading.
    void ExpectLoadRefused(const char* Path, const FGuid& Guid)
    {
        EXPECT_EQ(CPackage::LoadPackage(Path), nullptr) << Path;
        EXPECT_EQ(FindObject<CObject>(Guid), nullptr) << Path << ": a refused file must create no export";
        if (CPackage* Refused = CPackage::FindPackageByPath(Path))
        {
            RetirePackage(Refused);
        }
    }

    // Reports a size and range reads the way a dropping drive or a file still being written would.
    class FLyingFileSystem : public VFS::FMemoryFileSystem
    {
    public:

        using FMemoryFileSystem::FMemoryFileSystem;

        int64 Size(FStringView Path) const override
        {
            return SizeOverride >= 0 ? SizeOverride : FMemoryFileSystem::Size(Path);
        }

        bool ReadFileRange(TVector<uint8>& Result, FStringView Path, int64 Offset, int64 Size) override
        {
            return !(bFailTailReads && Offset != 0) && FMemoryFileSystem::ReadFileRange(Result, Path, Offset, Size);
        }

        int64 SizeOverride   = -1;
        bool  bFailTailReads = false;
    };
}

// packages saved before the codec word was added are deflate, and must keep reading
TEST(PackageCodec, AVersionOneDeflateContainerStillReads)
{
    EnsureCodecMount();

    const TVector<uint8> Payload = MakeCompressiblePayload(64 * 1024);

    const char* Path = "/PkgCodec/LegacyDeflate.lasset";
    ASSERT_TRUE(WriteBytes(Path, WrapInLegacyDeflateContainer(Payload)));

    TVector<uint8> Decompressed;
    ASSERT_TRUE(CPackage::ReadPackageFile(Path, Decompressed));
    EXPECT_EQ(Decompressed.Num(), Payload.Num());
    EXPECT_EQ(std::memcmp(Decompressed.GetData(), Payload.GetData(), Payload.Num()), 0);
}

TEST(PackageCodec, AnUnknownCodecIsRefusedRatherThanMisread)
{
    EnsureCodecMount();

    TVector<uint8> Container;
    AppendU32(Container, kChunkMagic);
    AppendU32(Container, 2);
    AppendU32(Container, 99);
    AppendU64(Container, 16);
    AppendU32(Container, uint32(4 * Constants::kMiB));
    AppendU32(Container, 1);
    AppendU32(Container, 8);
    Container.SetNum(Container.Num() + 8, 0);

    const char* Path = "/PkgCodec/UnknownCodec.lasset";
    ASSERT_TRUE(WriteBytes(Path, Container));

    TVector<uint8> Decompressed;
    EXPECT_FALSE(CPackage::ReadPackageFile(Path, Decompressed));
}

TEST(PackageCodec, AContainerFromAFutureVersionIsRefused)
{
    EnsureCodecMount();

    TVector<uint8> Container;
    AppendU32(Container, kChunkMagic);
    AppendU32(Container, 99);
    AppendU64(Container, 16);
    AppendU32(Container, uint32(4 * Constants::kMiB));
    AppendU32(Container, 1);
    AppendU32(Container, 8);
    Container.SetNum(Container.Num() + 8, 0);

    const char* Path = "/PkgCodec/FutureVersion.lasset";
    ASSERT_TRUE(WriteBytes(Path, Container));

    TVector<uint8> Decompressed;
    EXPECT_FALSE(CPackage::ReadPackageFile(Path, Decompressed));
}

// a real save, to prove what the engine actually writes now and that it reads its own output back
TEST(PackageCodec, ASavedPackageIsZstdAndRoundTrips)
{
    EnsureCodecMount();

    const char* Path = "/PkgCodec/Written.lasset";
    const FGuid Guid = SaveEmitterPackage(Path);

    TVector<uint8> Raw;
    ASSERT_TRUE(VFS::ReadFile(Raw, Path));
    ASSERT_GE(Raw.Num(), 12);
    EXPECT_EQ(ReadU32At(Raw, 0), kChunkMagic);
    EXPECT_EQ(ReadU32At(Raw, 4), 2u) << "a save must stamp the codec-bearing container version";
    EXPECT_EQ(ReadU32At(Raw, 8), 1u) << "and name zstd as the codec";

    CPackage* Loaded = CPackage::LoadPackage(Path);
    ASSERT_NE(Loaded, nullptr);
    ASSERT_TRUE(Loaded->FullyLoad());

    CParticleEmitter* Read = Cast<CParticleEmitter>(FindObject<CObject>(Guid));
    ASSERT_NE(Read, nullptr);
    EXPECT_FLOAT_EQ(Read->NoiseScale, 3.5f);
}

// A save interrupted by a crash or a dropped drive leaves a prefix of the file or a run of zeros where bytes should be.
TEST(PackageTruncation, ACutOrZeroFilledFileOnDiskIsRefused)
{
    EnsureCodecMount();
    TestUtil::FScopedTempMount Mount("/PkgTrunc", "LuminaPackageTruncationTests");

    const char* Path = "/PkgTrunc/Cut.lasset";
    const FGuid Guid = SaveEmitterPackage(Path);

    TVector<uint8> Whole;
    ASSERT_TRUE(VFS::ReadFile(Whole, Path));
    ASSERT_GT(Whole.Num(), 64);

    const int32 Cuts[] = { 0, 3, 8, 20, Whole.Num() / 2, Whole.Num() - 1 };
    for (int32 Cut : Cuts)
    {
        TVector<uint8> Prefix(Whole.GetData(), Whole.GetData() + Cut);
        ASSERT_TRUE(WriteBytes(Path, Prefix));

        TVector<uint8> Decompressed;
        EXPECT_FALSE(CPackage::ReadPackageFile(Path, Decompressed)) << "cut at " << Cut;
        ExpectLoadRefused(Path, Guid);
    }

    const int32 ZeroRuns[] = { 0, 8, 24, Whole.Num() / 2, Whole.Num() - 16 };
    for (int32 At : ZeroRuns)
    {
        TVector<uint8> Zeroed = Whole;
        Memory::Memzero(Zeroed.GetData() + At, 16);
        ASSERT_TRUE(WriteBytes(Path, Zeroed));
        ExpectLoadRefused(Path, Guid);
    }

    TVector<uint8> AllZeros(Whole.Num(), 0);
    ASSERT_TRUE(WriteBytes(Path, AllZeros));
    ExpectLoadRefused(Path, Guid);

    ASSERT_TRUE(WriteBytes(Path, Whole));
    CPackage* Loaded = CPackage::LoadPackage(Path);
    ASSERT_NE(Loaded, nullptr) << "the intact file must still load after every refusal";
    EXPECT_TRUE(Loaded->FullyLoad());
    RetirePackage(Loaded);
}

namespace
{
    // The container a save produced, with its header read back so a test can corrupt one field and rewrap it.
    struct FOpenedContainer
    {
        TVector<uint8> Bytes;
        FPackageHeader Header{};
    };

    FOpenedContainer OpenSavedContainer(const char* Path)
    {
        FOpenedContainer Opened;
        EXPECT_TRUE(CPackage::ReadPackageContainer(Path, Opened.Bytes, Opened.Header));
        EXPECT_EQ(Opened.Header.Tag, PACKAGE_FILE_TAG);
        return Opened;
    }

    void WriteCorruptedContainer(const char* Path, FOpenedContainer Opened)
    {
        TVector<uint8> HeaderBytes;
        FMemoryWriter Writer(HeaderBytes);
        Writer << Opened.Header;
        Memory::Memcpy(Opened.Bytes.GetData(), HeaderBytes.GetData(), HeaderBytes.Num());
        ASSERT_TRUE(WriteBytes(Path, WrapInLegacyDeflateContainer(Opened.Bytes)));
    }
}

// A header that decompresses fine can still point its tables past the bytes it came with.
TEST(PackageTruncation, TableOffsetsPastTheContainerAreRefusedByLoadAndCook)
{
    EnsureCodecMount();
    TestUtil::FScopedTempMount Mount("/PkgTrunc", "LuminaPackageTruncationTests");

    const char* Path = "/PkgTrunc/Offsets.lasset";
    const FGuid Guid = SaveEmitterPackage(Path);
    const FOpenedContainer Intact = OpenSavedContainer(Path);

    TVector<uint8> Cooked;
    WriteCorruptedContainer(Path, Intact);
    ASSERT_TRUE(CPackage::StripPackageForCook(Path, Cooked)) << "the rewrapped intact container must still cook";

    // The cooker's verbatim fallback and the strip both stand on this check, so it must refuse what they refuse.
    auto ExpectContainerRefused = [&]
    {
        TVector<uint8> Bytes;
        FPackageHeader Header;
        EXPECT_FALSE(CPackage::ReadPackageContainer(Path, Bytes, Header));
        EXPECT_FALSE(CPackage::StripPackageForCook(Path, Cooked));
        ExpectLoadRefused(Path, Guid);
    };

    FOpenedContainer ExportPastEnd = Intact;
    ExportPastEnd.Header.ExportTableOffset = Intact.Bytes.Num() + 100;
    WriteCorruptedContainer(Path, ExportPastEnd);
    ExpectContainerRefused();

    FOpenedContainer ImportNegative = Intact;
    ImportNegative.Header.ImportTableOffset = -1;
    WriteCorruptedContainer(Path, ImportNegative);
    ExpectContainerRefused();

    FOpenedContainer WrongTag = Intact;
    WrongTag.Header.Tag = PACKAGE_FILE_TAG ^ 1u;
    WriteCorruptedContainer(Path, WrongTag);
    ExpectContainerRefused();

    FOpenedContainer NamesPastEnd = Intact;
    NamesPastEnd.Header.NameTableOffset = Intact.Bytes.Num() + 1;
    WriteCorruptedContainer(Path, NamesPastEnd);
    EXPECT_FALSE(CPackage::StripPackageForCook(Path, Cooked));
    ExpectLoadRefused(Path, Guid);

    // In range, but the table's own count and entries run off the end of the bytes.
    FOpenedContainer ExportCutShort = Intact;
    ExportCutShort.Header.ExportTableOffset = Intact.Bytes.Num() - 2;
    WriteCorruptedContainer(Path, ExportCutShort);
    ExpectLoadRefused(Path, Guid);
}

// DestroyPackage must not seek into an export table a corrupt unloaded file never actually wrote.
TEST(PackageTruncation, DestroyOfACorruptedUnloadedPackageIsRefused)
{
    EnsureCodecMount();
    TestUtil::FScopedTempMount Mount("/PkgTrunc", "LuminaPackageTruncationTests");

    const char* Path = "/PkgTrunc/CodecEmitter.lasset";
    SaveEmitterPackage(Path);
    const FOpenedContainer Intact = OpenSavedContainer(Path);

    // The real primary export reads in full, then this claimed count runs the reader past the end of the file.
    FOpenedContainer InflatedExportCount = Intact;
    const int64 BytesAfterCount = Intact.Bytes.Num() - (Intact.Header.ExportTableOffset + (int64)sizeof(uint64));
    ASSERT_GT(BytesAfterCount, 0);
    const uint64 BogusExportCount = (uint64)BytesAfterCount;
    Memory::Memcpy(InflatedExportCount.Bytes.GetData() + Intact.Header.ExportTableOffset, &BogusExportCount, sizeof(BogusExportCount));
    WriteCorruptedContainer(Path, InflatedExportCount);
    EXPECT_FALSE(CPackage::DestroyPackage(Path));
    EXPECT_TRUE(VFS::Exists(Path)) << "a refused destroy must leave the file in place";

    WriteCorruptedContainer(Path, Intact);
    EXPECT_TRUE(CPackage::DestroyPackage(Path));
    EXPECT_FALSE(VFS::Exists(Path));
}

// The graph loader reaches the same file through the registry, and must come back empty-handed the same way.
TEST(PackageTruncation, AGraphLoadOfATruncatedRegisteredAssetReturnsNothing)
{
    EnsureCodecMount();
    TestUtil::FScopedTempMount Mount("/PkgTrunc", "LuminaPackageTruncationTests");

    const char* Path = "/PkgTrunc/Graph.lasset";
    const FGuid Guid = SaveEmitterPackage(Path, /*bRegisterAsset*/ true);
    ASSERT_NE(FAssetRegistry::Get().GetAssetByGUID(Guid), nullptr);

    TVector<uint8> Whole;
    ASSERT_TRUE(VFS::ReadFile(Whole, Path));
    Whole.SetNum(Whole.Num() / 2);
    ASSERT_TRUE(WriteBytes(Path, Whole));

    EXPECT_EQ(CPackage::LoadAssetGraph(Guid), nullptr);
    EXPECT_EQ(FindObject<CObject>(Guid), nullptr);

    FAssetRegistry::Get().AssetDeleted(Guid);
    if (CPackage* Refused = CPackage::FindPackageByPath(Path))
    {
        RetirePackage(Refused);
    }
}

// Above the whole-file limit the reader trusts a size query and a trailer read, which a dropping drive answers wrongly.
TEST(PackageTruncation, ALargeFileWhoseSizeOrTailReadDisagreesIsRefused)
{
    RegisterPendingTypes();

    const char* Alias = "/PkgLying";
    FLyingFileSystem& Lying = VFS::Mount<FLyingFileSystem>(Alias);

    const char* Path = "/PkgLying/Large.lasset";
    const TVector<uint8> Payload = MakeIncompressiblePayload(2 * Constants::kMiB);
    const TVector<uint8> Container = WrapInLegacyDeflateContainer(Payload);
    ASSERT_GT((uint64)Container.Num(), Constants::kMiB) << "the test must reach the large-file branch";
    ASSERT_TRUE(WriteBytes(Path, Container));

    TVector<uint8> Decompressed;
    ASSERT_TRUE(CPackage::ReadPackageFile(Path, Decompressed));
    EXPECT_EQ(Decompressed.Num(), Payload.Num());

    Lying.SizeOverride = 0;
    EXPECT_FALSE(CPackage::ReadPackageFile(Path, Decompressed)) << "a zero size after a megabyte was read means the drive dropped";

    Lying.SizeOverride = Container.Num() + 100;
    EXPECT_FALSE(CPackage::ReadPackageFile(Path, Decompressed)) << "a container read that comes back short must not be decompressed";

    Lying.SizeOverride   = -1;
    Lying.bFailTailReads = true;
    EXPECT_FALSE(CPackage::ReadPackageFile(Path, Decompressed)) << "a failed trailer read cannot be taken as no trailer";

    Lying.bFailTailReads = false;
    EXPECT_TRUE(CPackage::ReadPackageFile(Path, Decompressed));

    VFS::Unmount(Alias);
}
