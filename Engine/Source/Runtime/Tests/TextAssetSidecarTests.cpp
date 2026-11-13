#include <gtest/gtest.h>

#include "Assets/AssetRegistry/TextAssetSidecar.h"
#include "FileSystem/FileSystem.h"
#include "ScopedTempMountTestUtil.h"

using namespace Lumina;

TEST(TextAssetSidecar, MoveRelocatesTheGuidOnSuccess)
{
    TestUtil::FScopedTempMount Mount("/Game", "TextAssetSidecarMove");

    const FStringView OldPath("/Game/Content/Old/Doc.rml");
    const FStringView NewPath("/Game/Content/New/Doc.rml");
    const FGuid Guid = TextAssetSidecar::ReadOrMint(OldPath, ETextAssetKind::RmlDocument);

    EXPECT_TRUE(TextAssetSidecar::Move(OldPath, NewPath));

    FGuid Moved;
    EXPECT_FALSE(TextAssetSidecar::Read(OldPath, Moved));
    ASSERT_TRUE(TextAssetSidecar::Read(NewPath, Moved));
    EXPECT_EQ(Moved, Guid);
}

TEST(TextAssetSidecar, MoveLeavesTheOldSidecarWhenTheNewWriteFails)
{
    TestUtil::FScopedTempMount Mount("/Game", "TextAssetSidecarMoveFail");

    const FStringView OldPath("/Game/Content/Old/Doc.rml");
    const FStringView NewPath("/Game/Content/New/Doc.rml");
    const FGuid Guid = TextAssetSidecar::ReadOrMint(OldPath, ETextAssetKind::RmlDocument);

    const FFixedString NewSidecarPath = TextAssetSidecar::PathFor(NewPath);
    ASSERT_TRUE(VFS::WriteFile(FStringView(*NewSidecarPath, NewSidecarPath.Len()), FStringView("blocked")));
    const FPathString NewSidecarDiskPath = VFS::ResolvePath(FStringView(*NewSidecarPath, NewSidecarPath.Len()));
    ASSERT_TRUE(Filesystem::SetReadOnly(NewSidecarDiskPath, true));

    EXPECT_FALSE(TextAssetSidecar::Move(OldPath, NewPath));

    FGuid StillOld;
    ASSERT_TRUE(TextAssetSidecar::Read(OldPath, StillOld));
    EXPECT_EQ(StillOld, Guid);

    Filesystem::SetReadOnly(NewSidecarDiskPath, false);
}
