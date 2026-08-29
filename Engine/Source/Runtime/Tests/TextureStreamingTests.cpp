#include <gtest/gtest.h>

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Renderer/TextureData.h"

#include <filesystem>

using namespace Lumina;

// A WIRE FORMAT change, so these pin the actual bytes rather than just that it loaded.

namespace
{
    // Per-mip patterns catch a mis-addressed bulk read as wrong CONTENT, not wrong length.
    void FillMip(FTextureResource::FMip& Mip, uint32 Width, uint32 Height, uint8 Seed)
    {
        Mip.Width      = Width;
        Mip.Height     = Height;
        Mip.Depth      = 1;
        Mip.RowPitch   = Width * 4;
        Mip.SlicePitch = Width * Height * 4;

        Mip.Pixels.resize((size_t)Width * Height * 4);
        for (size_t i = 0; i < Mip.Pixels.size(); ++i)
        {
            Mip.Pixels[i] = (uint8)((i * 31u + Seed * 7u) & 0xFF);
        }
    }

    // Mips 0 and 1 are above the 256 inline threshold and stream; 2 through 10 are inline.
    void BuildChain(FTextureResource& Resource, uint32 BaseSize = 1024)
    {
        uint32 NumMips = 1;
        for (uint32 S = BaseSize; S > 1; S >>= 1)
        {
            ++NumMips;
        }

        Resource.ImageDescription.Extent     = FUIntVector2(BaseSize, BaseSize);
        Resource.ImageDescription.NumMips    = (uint8)NumMips;
        Resource.ImageDescription.Format     = EFormat::RGBA8_UNORM;
        Resource.ImageDescription.LayerCount = 1;

        Resource.Mips.resize(NumMips);
        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            const uint32 Size = BaseSize >> Mip;
            FillMip(Resource.Mips[Mip], Size > 0 ? Size : 1u, Size > 0 ? Size : 1u, (uint8)(Mip + 1));
        }
    }

    // Scoped writable mount + on-disk directory for the package under test.
    struct FScopedPackageMount
    {
        FFixedString Alias = "/StreamTest";
        std::filesystem::path Dir;

        FScopedPackageMount()
        {
            Dir = std::filesystem::temp_directory_path() / "LuminaTextureStreamingTests";
            std::filesystem::remove_all(Dir);
            std::filesystem::create_directories(Dir);

            VFS::Mount<VFS::FNativeFileSystem>(Alias, FStringView(Dir.string().c_str()));
        }

        ~FScopedPackageMount()
        {
            VFS::Unmount(Alias);
            std::error_code Ec;
            std::filesystem::remove_all(Dir, Ec);
        }
    };
}

TEST(TextureStreaming, ComputeFirstInlineMipSplitsAtThreshold)
{
    FTextureResource::FDescription Desc;
    Desc.Extent     = FUIntVector2(4096, 4096);
    Desc.NumMips    = 13;   // 4096 .. 1
    Desc.LayerCount = 1;

    // 4096, 2048, 1024, 512 stream; 256 and below stay inline.
    EXPECT_EQ(FTextureResource::ComputeFirstInlineMip(Desc), 4);

    Desc.Extent  = FUIntVector2(256, 256);
    Desc.NumMips = 9;
    EXPECT_EQ(FTextureResource::ComputeFirstInlineMip(Desc), 0)
        << "a texture already at the threshold has nothing worth streaming";

    Desc.Extent  = FUIntVector2(64, 64);
    Desc.NumMips = 7;
    EXPECT_EQ(FTextureResource::ComputeFirstInlineMip(Desc), 0);

    // Non-square, so the LONG edge decides or a 4096x64 texture reads as tiny.
    Desc.Extent  = FUIntVector2(4096, 64);
    Desc.NumMips = 13;
    EXPECT_EQ(FTextureResource::ComputeFirstInlineMip(Desc), 4);

    Desc.Extent     = FUIntVector2(4096, 4096);
    Desc.NumMips    = 13;
    Desc.LayerCount = 6;
    EXPECT_EQ(FTextureResource::ComputeFirstInlineMip(Desc), 4)
        << "arrays stream on the same split as 2D since Recreate gained an overload that repoints the slot";

    // A chain stopping above the threshold still keeps its smallest mip inline.
    Desc.Extent     = FUIntVector2(4096, 4096);
    Desc.NumMips    = 2;   // 4096 and 2048, both above 256
    Desc.LayerCount = 1;
    EXPECT_EQ(FTextureResource::ComputeFirstInlineMip(Desc), 1);
}

TEST(TextureStreaming, NonPackageArchiveKeepsEveryMipInline)
{
    // FMemoryWriter has nowhere to put a bulk region, so it must fall back to fully inline.
    FTextureResource Source;
    BuildChain(Source);

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes, false);
        ASSERT_FALSE(Writer.SupportsBulkData());
        Writer << Source;
    }

    EXPECT_EQ(Source.ImageDescription.FirstInlineMip, 0);

    FTextureResource Loaded;
    FMemoryReader Reader(Bytes);
    Reader << Loaded;

    ASSERT_EQ(Loaded.Mips.size(), Source.Mips.size());
    EXPECT_EQ(Loaded.ImageDescription.FirstInlineMip, 0);
    EXPECT_FALSE(Loaded.IsStreamable());

    for (size_t i = 0; i < Loaded.Mips.size(); ++i)
    {
        EXPECT_EQ(Loaded.Mips[i].Pixels, Source.Mips[i].Pixels) << "mip " << i << " did not round-trip";
    }
}

TEST(TextureStreaming, PackageSaveWritesRecoverableBulkMips)
{
    FScopedPackageMount Mount;

    CPackage* Package = CPackage::CreatePackage("/StreamTest/BulkTexture");
    ASSERT_NE(Package, nullptr);

    CTexture* Texture = NewObject<CTexture>(Package, "BulkTexture");
    ASSERT_NE(Texture, nullptr);

    Texture->TextureResource = MakeUnique<FTextureResource>();
    BuildChain(*Texture->TextureResource);

    // Saving may move the mips out of the live object, so keep a copy of the source bytes.
    TVector<TVector<uint8>> Expected;
    for (const FTextureResource::FMip& Mip : Texture->TextureResource->Mips)
    {
        Expected.push_back(Mip.Pixels);
    }

    const FFixedString Path = Package->GetPackagePath();
    ASSERT_TRUE(CPackage::SavePackage(Package, Path));

    // The split is decided at save time from the description alone.
    const uint8 FirstInlineMip = Texture->TextureResource->ImageDescription.FirstInlineMip;
    ASSERT_EQ(FirstInlineMip, 2) << "1024 and 512 should stream; 256 and below inline";

    // The package now knows where its bulk region is, and it holds exactly the streamed mips.
    const CPackage::FBulkRegion& Region = Package->GetBulkRegion();
    ASSERT_TRUE(Region.IsValid());

    uint64 ExpectedBulkBytes = 0;
    for (uint32 Mip = 0; Mip < FirstInlineMip; ++Mip)
    {
        ExpectedBulkBytes += Expected[Mip].size();
    }
    EXPECT_EQ((uint64)Region.Size, ExpectedBulkBytes);

    // Content, not just length, so a swapped or overlapping offset fails here.
    for (uint32 Mip = 0; Mip < FirstInlineMip; ++Mip)
    {
        const FBulkDataRef& Ref = Texture->TextureResource->Mips[Mip].BulkRef;
        ASSERT_TRUE(Ref.IsValid()) << "mip " << Mip << " has no bulk ref";
        EXPECT_EQ((uint64)Ref.Size, (uint64)Expected[Mip].size());

        TVector<uint8> Read;
        ASSERT_TRUE(Package->ReadBulkData(Ref, Read)) << "mip " << Mip << " could not be read back";
        EXPECT_EQ(Read, Expected[Mip]) << "mip " << Mip << " bytes differ";
    }

    // Inline mips must NOT have been diverted to the region.
    for (uint32 Mip = FirstInlineMip; Mip < (uint32)Texture->TextureResource->Mips.size(); ++Mip)
    {
        EXPECT_FALSE(Texture->TextureResource->Mips[Mip].BulkRef.IsValid())
            << "mip " << Mip << " is inline and should carry no bulk ref";
    }

    // Reading the package back must yield only the container, with mip bytes still on disk.
    TVector<uint8>          Container;
    CPackage::FBulkRegion   ReadRegion;
    ASSERT_TRUE(CPackage::ReadPackageFile(Path, Container, &ReadRegion));
    EXPECT_EQ(ReadRegion.FileOffset, Region.FileOffset);
    EXPECT_EQ(ReadRegion.Size, Region.Size);
    EXPECT_LT((uint64)Container.size(), ExpectedBulkBytes)
        << "the inflated container should be far smaller than the mips it no longer holds";

    ASSERT_TRUE(CPackage::DestroyPackage(Package));
}

TEST(TextureStreaming, ReSaveAfterStreamOutPreservesMips)
{
    // A texture whose streamed mips were evicted has empty Pixels, which PreSave must catch.
    FScopedPackageMount Mount;

    CPackage* Package = CPackage::CreatePackage("/StreamTest/ResaveTexture");
    ASSERT_NE(Package, nullptr);

    CTexture* Texture = NewObject<CTexture>(Package, "ResaveTexture");
    ASSERT_NE(Texture, nullptr);

    Texture->TextureResource = MakeUnique<FTextureResource>();
    BuildChain(*Texture->TextureResource);

    TVector<TVector<uint8>> Expected;
    for (const FTextureResource::FMip& Mip : Texture->TextureResource->Mips)
    {
        Expected.push_back(Mip.Pixels);
    }

    const FFixedString Path = Package->GetPackagePath();
    ASSERT_TRUE(CPackage::SavePackage(Package, Path));

    const uint8 FirstInlineMip = Texture->TextureResource->ImageDescription.FirstInlineMip;
    ASSERT_GT(FirstInlineMip, 0);

    // Simulate the streamer evicting every streamed mip's CPU copy.
    for (uint32 Mip = 0; Mip < FirstInlineMip; ++Mip)
    {
        Texture->TextureResource->Mips[Mip].Pixels.clear();
        Texture->TextureResource->Mips[Mip].Pixels.shrink_to_fit();
    }

    ASSERT_TRUE(CPackage::SavePackage(Package, Path));

    // After the re-save the region must still hold the real bytes, at the new offsets.
    for (uint32 Mip = 0; Mip < FirstInlineMip; ++Mip)
    {
        const FBulkDataRef& Ref = Texture->TextureResource->Mips[Mip].BulkRef;
        ASSERT_TRUE(Ref.IsValid());

        TVector<uint8> Read;
        ASSERT_TRUE(Package->ReadBulkData(Ref, Read));
        EXPECT_EQ(Read, Expected[Mip]) << "mip " << Mip << " was lost across a stream-out + re-save";
    }

    ASSERT_TRUE(CPackage::DestroyPackage(Package));
}
