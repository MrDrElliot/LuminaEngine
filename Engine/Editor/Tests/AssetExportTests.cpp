#include <gtest/gtest.h>

#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Tools/Export/AssetExport.h"
#include "World/World.h"

using namespace Lumina;

namespace
{
    // Removed on both ends, so a failed earlier run cannot leave files that change this one's names.
    struct FExportFolder
    {
        FString Root = FString(*Paths::Combine(Filesystem::GetTempDirectory(), "LuminaAssetExportTests"));

        FExportFolder()
        {
            Filesystem::RemoveTree(Root);
            Filesystem::MakeDirectoryTree(Root);
        }

        ~FExportFolder() { Filesystem::RemoveTree(Root); }
    };

    CAudioStream* NewWaveStream(const char* Name)
    {
        CAudioStream* Stream = NewObject<CAudioStream>(nullptr, FName(Name), FGuid::New(), OF_Transient);
        Stream->AudioData = MakeShared<FAudioData>();
        const char Header[] = "RIFF\x24\x00\x00\x00WAVEfmt ";
        Stream->AudioData->Bytes.Reset();
        Stream->AudioData->Bytes.Append(Header, Header + sizeof(Header) - 1);
        return Stream;
    }
}

TEST(AssetExport, OnlyTypesThatKeepTheirSourceCanExport)
{
    EXPECT_TRUE(AssetExport::CanExport(CAudioStream::StaticClass()));
    EXPECT_FALSE(AssetExport::CanExport(CWorld::StaticClass()));
    EXPECT_FALSE(AssetExport::CanExport(nullptr));
}

TEST(AssetExport, WritesTheImportedBytesUnderTheAssetNameWithoutOverwriting)
{
    FExportFolder Folder;
    CAudioStream* Stream = NewWaveStream("Footstep");

    FString First;
    FString Second;
    FString Error;
    ASSERT_TRUE(AssetExport::Export(Stream, FStringView(*Folder.Root, Folder.Root.Len()), First, Error)) << *Error;
    ASSERT_TRUE(AssetExport::Export(Stream, FStringView(*Folder.Root, Folder.Root.Len()), Second, Error)) << *Error;

    EXPECT_TRUE(First.EndsWith("Footstep.wav")) << *First << " takes its extension from the RIFF signature";
    EXPECT_TRUE(Second.EndsWith("Footstep_1.wav")) << *Second;

    TVector<uint8> Written;
    ASSERT_TRUE(FileHelper::LoadFileToArray(Written, *First));
    EXPECT_EQ(Written, Stream->AudioData->Bytes);
}

TEST(AssetExport, AnAssetWithNothingToWriteReportsWhy)
{
    FExportFolder Folder;
    CAudioStream* Empty = NewObject<CAudioStream>(nullptr, FName("Silent"), FGuid::New(), OF_Transient);
    CWorld* World = NewObject<CWorld>(nullptr, FName("NotAnAsset"), FGuid::New(), OF_Transient);

    FString Written;
    FString Error;
    EXPECT_FALSE(AssetExport::Export(Empty, FStringView(*Folder.Root, Folder.Root.Len()), Written, Error));
    EXPECT_FALSE(Error.IsEmpty());
    EXPECT_FALSE(AssetExport::Export(World, FStringView(*Folder.Root, Folder.Root.Len()), Written, Error));
    EXPECT_FALSE(AssetExport::Export(nullptr, FStringView(*Folder.Root, Folder.Root.Len()), Written, Error));
}
