#include <gtest/gtest.h>

#include <string>

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/SoftObjectPtr.h"

using namespace Lumina;

namespace
{
    std::string ToStd(FStringView View)
    {
        return std::string(View.GetData(), View.Len());
    }

    std::string ToStd(const FFixedString& Str)
    {
        return std::string(*Str, Str.Len());
    }
}

TEST(SoftObjectPath, ResolveFollowsRenameByGUID)
{
    CPackage* Package = CPackage::CreatePackage("/SoftPathTest/OldName");
    ASSERT_NE(Package, nullptr);

    CDataTable* Table = NewObject<CDataTable>(Package, "OldName");
    ASSERT_NE(Table, nullptr);

    FAssetRegistry& Registry = FAssetRegistry::Get();
    Registry.AssetCreated(Table);

    const FAssetData* Data = Registry.GetAssetByGUID(Table->GetGUID());
    ASSERT_NE(Data, nullptr);

    const FFixedString OldPath = Data->Path;
    FFixedString NewPath = OldPath;
    const int32 NamePos = NewPath.Find("OldName");
    ASSERT_NE(NamePos, Constants::kIndexNone);
    NewPath.RemoveAt(NamePos, 7).InsertAt(NamePos, "NewName");

    FSoftObjectPath Soft{ FStringView(*OldPath, OldPath.Len()) };
    ASSERT_TRUE(Soft.TryResolve());
    ASSERT_TRUE(Soft.GetCachedGUID() == Table->GetGUID());

    Registry.AssetRenamed(FStringView(*OldPath, OldPath.Len()),
                          FStringView(*NewPath, NewPath.Len()));

    EXPECT_TRUE(Soft.TryResolve());
    EXPECT_EQ(ToStd(Soft.GetPath()), ToStd(NewPath));

    Registry.AssetDeleted(Table->GetGUID());
}

TEST(SoftObjectPath, ResolveAdoptsAnAssetRecreatedAtTheSamePath)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();

    CDataTable* First = NewObject<CDataTable>(CPackage::CreatePackage("/SoftPathTest/Recreated"), "Recreated");
    ASSERT_NE(First, nullptr);
    Registry.AssetCreated(First);
    const FFixedString Path = Registry.GetAssetByGUID(First->GetGUID())->Path;

    FSoftObjectPath Soft{ FStringView(*Path, Path.Len()) };
    ASSERT_TRUE(Soft.TryResolve());
    ASSERT_TRUE(Soft.GetCachedGUID() == First->GetGUID());
    Registry.AssetDeleted(First->GetGUID());

    CDataTable* Second = NewObject<CDataTable>(CPackage::CreatePackage("/SoftPathTest/StandIn"), "StandIn");
    ASSERT_NE(Second, nullptr);
    Registry.AssetCreated(Second);
    const FFixedString StandInPath = Registry.GetAssetByGUID(Second->GetGUID())->Path;
    Registry.AssetRenamed(FStringView(*StandInPath, StandInPath.Len()), FStringView(*Path, Path.Len()));

    EXPECT_TRUE(Soft.TryResolve());
    EXPECT_TRUE(Soft.GetCachedGUID() == Second->GetGUID());

    Registry.AssetDeleted(Second->GetGUID());
}

TEST(SoftObjectPath, ResolveKeepsPathWhenGUIDHasNoRegistryEntry)
{
    FSoftObjectPath Soft{ FStringView("/SoftPathTest/Absent"), FGuid::New() };

    EXPECT_TRUE(Soft.TryResolve());
    EXPECT_EQ(ToStd(Soft.GetPath()), std::string("/SoftPathTest/Absent"));
}
