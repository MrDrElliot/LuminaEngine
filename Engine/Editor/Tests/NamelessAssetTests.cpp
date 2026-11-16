#include <gtest/gtest.h>

#include "Asset/AssetOps.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"

using namespace Lumina;

namespace
{
    constexpr const char* kAlias = "/NamelessAssetTests";

    struct FScopedMemoryMount
    {
        FScopedMemoryMount()
        {
            RegisterPendingTypes();
            VFS::Unmount(kAlias);
            VFS::Mount<VFS::FMemoryFileSystem>(kAlias);
        }

        ~FScopedMemoryMount()
        {
            VFS::Unmount(kAlias);
        }
    };
}

TEST(NamelessAsset, APathNamingOnlyAFolderCreatesNoPackage)
{
    FScopedMemoryMount Mount;
    EXPECT_EQ(CPackage::CreatePackage("/NamelessAssetTests/Mech01/"), nullptr);
    EXPECT_EQ(CPackage::CreatePackage("/NamelessAssetTests/Mech01/.lasset"), nullptr);
    EXPECT_EQ(VFS::FileName("SM_Mech.glb", true), "SM_Mech");
}

// A file whose object does not carry the file's name never loads or registers, so deleting it is the only way out.
TEST(NamelessAsset, AFileWhoseObjectDoesNotCarryItsNameCanStillBeDeleted)
{
    FScopedMemoryMount Mount;
    constexpr const char* kSaved = "/NamelessAssetTests/Mech01/SM_Mech";
    constexpr const char* kNameless = "/NamelessAssetTests/Mech01/.lasset";

    CPackage* Package = CPackage::CreatePackage(kSaved);
    ASSERT_NE(Package, nullptr);
    NewObject<CStaticMesh>(Package, FName("SM_Mech"));
    ASSERT_TRUE(CPackage::SavePackage(Package, "/NamelessAssetTests/Mech01/SM_Mech.lasset"));

    TVector<uint8> Bytes;
    ASSERT_TRUE(VFS::ReadFile(Bytes, "/NamelessAssetTests/Mech01/SM_Mech.lasset"));
    ASSERT_TRUE(VFS::WriteFile(kNameless, TSpan<const uint8>(Bytes.GetData(), Bytes.Num())));

    const AssetOps::FDeleteAssetResult Result = AssetOps::DeleteAsset(kNameless, nullptr);
    EXPECT_TRUE(Result.bDeleted) << *Result.Error;
    EXPECT_FALSE(VFS::Exists(kNameless));
    EXPECT_TRUE(VFS::Exists("/NamelessAssetTests/Mech01/SM_Mech.lasset"));
    EXPECT_TRUE(AssetOps::DeleteAsset(kSaved, nullptr).bDeleted);
}
