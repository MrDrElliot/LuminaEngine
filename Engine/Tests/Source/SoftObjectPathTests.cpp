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
        return std::string(View.data(), View.size());
    }

    std::string ToStd(const FFixedString& Str)
    {
        return std::string(Str.c_str(), Str.size());
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
    const size_t NamePos = NewPath.find("OldName");
    ASSERT_NE(NamePos, FFixedString::npos);
    NewPath.replace(NamePos, 7, "NewName");

    FSoftObjectPath Soft{ FStringView(OldPath.c_str(), OldPath.size()) };
    ASSERT_TRUE(Soft.TryResolve());
    ASSERT_TRUE(Soft.GetCachedGUID() == Table->GetGUID());

    Registry.AssetRenamed(FStringView(OldPath.c_str(), OldPath.size()),
                          FStringView(NewPath.c_str(), NewPath.size()));

    EXPECT_TRUE(Soft.TryResolve());
    EXPECT_EQ(ToStd(Soft.GetPath()), ToStd(NewPath));

    Registry.AssetDeleted(Table->GetGUID());
}

TEST(SoftObjectPath, ResolveKeepsPathWhenGUIDHasNoRegistryEntry)
{
    FSoftObjectPath Soft{ FStringView("/SoftPathTest/Absent"), FGuid::New() };

    EXPECT_TRUE(Soft.TryResolve());
    EXPECT_EQ(ToStd(Soft.GetPath()), std::string("/SoftPathTest/Absent"));
}
