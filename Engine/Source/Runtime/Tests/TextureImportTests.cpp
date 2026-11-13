#include <gtest/gtest.h>

#include "Containers/StringFormat.h"
#include "GUID/GUID.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Tools/Image/ImageWrite.h"
#include "Tools/Import/ImportHelpers.h"

using namespace Lumina;

TEST(TextureImport, ImportResizesAnHDRSourceToTargetSize)
{
    const FString Path = Format("{}/LuminaTextureImportTest_{}.hdr", Filesystem::GetTempDirectory(), FGuid::New());

    constexpr uint32 SrcWidth = 4;
    constexpr uint32 SrcHeight = 4;
    TVector<float> Pixels(SrcWidth * SrcHeight * 3, 0.5f);
    ASSERT_TRUE(ImageWrite::WriteHdrFile(Path.GetData(), SrcWidth, SrcHeight, 3, Pixels.GetData()));

    const FUIntVector2 TargetSize(2, 2);
    TOptional<Import::Textures::FTextureImportResult> Result = Import::Textures::ImportTexture(Path, false, TargetSize);
    Filesystem::RemoveFile(Path);

    ASSERT_TRUE(Result.IsSet());
    EXPECT_EQ(Result->Dimensions.x, TargetSize.x);
    EXPECT_EQ(Result->Dimensions.y, TargetSize.y);
}

TEST(TextureImport, HDRChannelFormatCoversEveryStbChannelCount)
{
    EXPECT_EQ(Import::Textures::GetHDRChannelFormat(1), EFormat::R32_FLOAT);
    EXPECT_EQ(Import::Textures::GetHDRChannelFormat(2), EFormat::RG32_FLOAT);
    EXPECT_EQ(Import::Textures::GetHDRChannelFormat(3), EFormat::RGB32_FLOAT);
    EXPECT_EQ(Import::Textures::GetHDRChannelFormat(4), EFormat::RGBA32_FLOAT);
}

// stb never decodes an HDR image past 4 channels, but the mapping must still refuse one anyway.
TEST(TextureImport, HDRChannelFormatRejectsAnUnsupportedChannelCount)
{
    EXPECT_FALSE(Import::Textures::GetHDRChannelFormat(5).IsSet());
    EXPECT_FALSE(Import::Textures::GetHDRChannelFormat(0).IsSet());
}
