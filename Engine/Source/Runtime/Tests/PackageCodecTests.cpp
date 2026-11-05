#include <gtest/gtest.h>

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
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
}

// packages saved before the codec word was added are deflate, and must keep reading
TEST(PackageCodec, AVersionOneDeflateContainerStillReads)
{
    EnsureCodecMount();

    const TVector<uint8> Payload = MakeCompressiblePayload(64 * 1024);

    mz_ulong Bound = mz_compressBound((mz_ulong)Payload.Num());
    TVector<uint8> Deflated(Lumina::IntCastChecked<Lumina::int32>(Bound));
    mz_ulong DeflatedLength = Bound;
    ASSERT_EQ(mz_compress2(Deflated.GetData(), &DeflatedLength, Payload.GetData(),
                           (mz_ulong)Payload.Num(), MZ_DEFAULT_LEVEL), MZ_OK);
    Deflated.SetNum(Lumina::IntCastChecked<Lumina::int32>(DeflatedLength));

    // the v1 layout, which carried no codec word because deflate was the only one
    TVector<uint8> Container;
    AppendU32(Container, kChunkMagic);
    AppendU32(Container, 1);
    AppendU64(Container, Payload.Num());
    AppendU32(Container, uint32(4 * Constants::kMiB));
    AppendU32(Container, 1);
    AppendU32(Container, (uint32)Deflated.Num());
    Container.Append(Deflated);

    const char* Path = "/PkgCodec/LegacyDeflate.lasset";
    ASSERT_TRUE(VFS::WriteFile(Path, TSpan<const uint8>(Container.GetData(), Container.Num())));

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
    ASSERT_TRUE(VFS::WriteFile(Path, TSpan<const uint8>(Container.GetData(), Container.Num())));

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
    ASSERT_TRUE(VFS::WriteFile(Path, TSpan<const uint8>(Container.GetData(), Container.Num())));

    TVector<uint8> Decompressed;
    EXPECT_FALSE(CPackage::ReadPackageFile(Path, Decompressed));
}

// a real save, to prove what the engine actually writes now and that it reads its own output back
TEST(PackageCodec, ASavedPackageIsZstdAndRoundTrips)
{
    EnsureCodecMount();
    CParticleEmitter::StaticClass()->Link();

    const char* Path = "/PkgCodec/Written.lasset";
    const FGuid Guid = FGuid::New();

    {
        CPackage* Package = CPackage::CreatePackage(Path);
        ASSERT_NE(Package, nullptr);

        CParticleEmitter* Emitter = Cast<CParticleEmitter>(
            NewObject(CParticleEmitter::StaticClass(), Package, "CodecEmitter", Guid));
        ASSERT_NE(Emitter, nullptr);
        Emitter->NoiseScale = 3.5f;

        ASSERT_TRUE(CPackage::SavePackage(Package, Path));

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
