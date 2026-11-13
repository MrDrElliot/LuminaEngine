#include <gtest/gtest.h>

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include <thread>

using namespace Lumina;

// A replace import writes a new asset with a fresh GUID into a file the registry already lists.
TEST(AssetRegistryReplace, ANewAssetAtAKnownPathReplacesTheOldEntry)
{
    const char* Path = "/RegistryReplace/Table";
    CPackage* Package = CPackage::CreatePackage(Path);
    ASSERT_NE(Package, nullptr);

    FAssetRegistry& Registry = FAssetRegistry::Get();

    CDataTable* Original = NewObject<CDataTable>(Package, "Table");
    ASSERT_NE(Original, nullptr);
    Registry.AssetCreated(Original);
    ASSERT_NE(Registry.GetAssetByPath(Path), nullptr);

    CDataTable* Replacement = NewObject<CDataTable>(Package, "TableReplacement");
    ASSERT_NE(Replacement, nullptr);
    ASSERT_NE(Original->GetGUID(), Replacement->GetGUID());
    Registry.AssetCreated(Replacement);

    const FAssetDataPtr ByPath = Registry.GetAssetByPath(Path);
    ASSERT_NE(ByPath, nullptr);
    EXPECT_EQ(ByPath->AssetGUID, Replacement->GetGUID());
    EXPECT_EQ(Registry.GetAssetByGUID(Original->GetGUID()), nullptr);

    const TVector<FAssetDataPtr> AtPath = Registry.FindByPredicate([&](const FAssetData& Data)
    {
        return Data.Path == ByPath->Path;
    });
    EXPECT_EQ(AtPath.Num(), 1);

    Registry.AssetDeleted(Replacement->GetGUID());
}

// Creating the same asset again must not leave a second entry behind under its old path.
TEST(AssetRegistryReplace, ReRegisteringAnAssetKeepsOneEntry)
{
    const char* Path = "/RegistryReplace/Again";
    CPackage* Package = CPackage::CreatePackage(Path);
    ASSERT_NE(Package, nullptr);

    FAssetRegistry& Registry = FAssetRegistry::Get();

    CDataTable* Table = NewObject<CDataTable>(Package, "Again");
    ASSERT_NE(Table, nullptr);
    Registry.AssetCreated(Table);
    Registry.AssetCreated(Table);

    const TVector<FAssetDataPtr> WithGuid = Registry.FindByPredicate([&](const FAssetData& Data)
    {
        return Data.AssetGUID == Table->GetGUID();
    });
    EXPECT_EQ(WithGuid.Num(), 1);
    EXPECT_NE(Registry.GetAssetByPath(Path), nullptr);

    Registry.AssetDeleted(Table->GetGUID());
}

// A rename lists a new entry rather than editing the old one, so a reader holding the old one never sees its fields change under it.
TEST(AssetRegistryReplace, ARenameOnAnotherThreadLeavesAHeldEntryUnchanged)
{
    const char* Path = "/RegistryReplace/Renamed";
    CPackage* Package = CPackage::CreatePackage(Path);
    ASSERT_NE(Package, nullptr);

    FAssetRegistry& Registry = FAssetRegistry::Get();

    CDataTable* Table = NewObject<CDataTable>(Package, "Renamed");
    ASSERT_NE(Table, nullptr);
    Registry.AssetCreated(Table);

    const FAssetDataPtr Held = Registry.GetAssetByGUID(Table->GetGUID());
    ASSERT_NE(Held, nullptr);
    const FFixedString OldPath = Held->Path;
    const FName OldName = Held->AssetName;
    const FFixedString NewPath("/RegistryReplace/RenamedTo.lasset");

    std::thread Renamer([&]
    {
        Registry.AssetRenamed(FStringView(*OldPath, OldPath.Len()), FStringView(*NewPath, NewPath.Len()));
    });
    Renamer.join();

    EXPECT_EQ(Held->Path, OldPath);
    EXPECT_EQ(Held->AssetName, OldName);

    const FAssetDataPtr Current = Registry.GetAssetByGUID(Table->GetGUID());
    ASSERT_NE(Current, nullptr);
    EXPECT_EQ(Current->Path, NewPath);
    EXPECT_EQ(Registry.GetAssetByPath(NewPath), Current);
    EXPECT_EQ(Registry.GetAssetByPath(OldPath), nullptr);

    Registry.AssetDeleted(Table->GetGUID());
}

// A dot in a folder name is not an extension, so listing an asset under it leaves the asset the folder name looks like alone.
TEST(AssetRegistryReplace, AnAssetUnderADottedFolderKeepsItsLookalikeSibling)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();

    const char* SiblingPath = "/RegistryDotted/v1";
    CDataTable* Sibling = NewObject<CDataTable>(CPackage::CreatePackage(SiblingPath), "v1");
    ASSERT_NE(Sibling, nullptr);
    Registry.AssetCreated(Sibling);

    CDataTable* Nested = NewObject<CDataTable>(CPackage::CreatePackage("/RegistryDotted/v1.2/Mesh"), "Mesh");
    ASSERT_NE(Nested, nullptr);
    Registry.AssetCreated(Nested);

    const FAssetDataPtr Found = Registry.GetAssetByPath(SiblingPath);
    ASSERT_NE(Found, nullptr);
    EXPECT_EQ(Found->AssetGUID, Sibling->GetGUID());
    EXPECT_NE(Registry.GetAssetByGUID(Sibling->GetGUID()), nullptr);

    Registry.AssetDeleted(Nested->GetGUID());
    Registry.AssetDeleted(Sibling->GetGUID());
}
