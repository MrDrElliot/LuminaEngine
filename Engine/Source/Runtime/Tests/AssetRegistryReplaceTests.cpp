#include <gtest/gtest.h>

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"

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

    const FAssetData* ByPath = Registry.GetAssetByPath(Path);
    ASSERT_NE(ByPath, nullptr);
    EXPECT_EQ(ByPath->AssetGUID, Replacement->GetGUID());
    EXPECT_EQ(Registry.GetAssetByGUID(Original->GetGUID()), nullptr);

    const TVector<FAssetData*> AtPath = Registry.FindByPredicate([&](const FAssetData& Data)
    {
        return Data.Path == ByPath->Path;
    });
    EXPECT_EQ(AtPath.size(), 1u);

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

    const TVector<FAssetData*> WithGuid = Registry.FindByPredicate([&](const FAssetData& Data)
    {
        return Data.AssetGUID == Table->GetGUID();
    });
    EXPECT_EQ(WithGuid.size(), 1u);
    EXPECT_NE(Registry.GetAssetByPath(Path), nullptr);

    Registry.AssetDeleted(Table->GetGUID());
}
