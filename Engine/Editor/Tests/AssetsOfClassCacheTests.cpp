#include <gtest/gtest.h>

#include "AssetRegistryTestAccess.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Class.h"
#include "UI/Properties/Customizations/AssetPickerFilter.h"

using namespace Lumina;

namespace
{
    bool ListsGuid(const TVector<FAssetDataPtr>& Assets, const FGuid& Guid)
    {
        return Algo::AnyOf(Assets, [&Guid](const FAssetDataPtr& Asset) { return Asset->AssetGUID == Guid; });
    }
}

TEST(AssetsOfClassCache, ReusesListUntilRegistryBroadcasts)
{
    CDataTable::StaticClass();
    RegisterPendingTypes();

    FAssetRegistry& Registry = FAssetRegistry::Get();
    AssetPickerFilter::FAssetsOfClassCache Cache;

    const auto CountBefore = Cache.Get(CDataTable::StaticClass()).Num();

    // Listed without a broadcast, so only a rescan can see it.
    const FGuid Guid = FGuid::New();
    FAssetRegistryTestAccess::ReplaceEntry(Registry, FAssetData{
        .AssetGUID = Guid,
        .Path = FFixedString("/AssetsOfClassCache/PickerCacheTable"),
        .AssetName = FName("PickerCacheTable"),
        .AssetClass = CDataTable::StaticClass()->GetName() });

    const TVector<FAssetDataPtr>& Cached = Cache.Get(CDataTable::StaticClass());
    EXPECT_EQ(Cached.Num(), CountBefore);
    EXPECT_FALSE(ListsGuid(Cached, Guid));

    Registry.GetOnAssetRegistryUpdated().Broadcast();

    const TVector<FAssetDataPtr>& Rescanned = Cache.Get(CDataTable::StaticClass());
    EXPECT_EQ(Rescanned.Num(), CountBefore + 1);
    EXPECT_TRUE(ListsGuid(Rescanned, Guid));

    EXPECT_FALSE(ListsGuid(Cache.Get(CClass::StaticClass()), Guid));

    Registry.AssetDeleted(Guid);
}
