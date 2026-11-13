#include <gtest/gtest.h>

#include "AssetRegistryTestAccess.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "TaskSystem/ThreadedCallback.h"
#include <thread>

using namespace Lumina;

namespace
{
    FAssetDataPtr CreateAssetAt(FAssetRegistry& Registry, const char* Path)
    {
        CPackage* Package = CPackage::CreatePackage(Path);
        CDataTable* Table = NewObject<CDataTable>(Package, "Table");
        Registry.AssetCreated(Table);
        return Registry.GetAssetByPath(Path);
    }
}

// A path like "/DLSSFrameGen/Foo" shares a string prefix with the walked root "/DLSS" without being nested under it.
TEST(AssetRegistryReap, ASiblingPathThatSharesAPrefixWithAWalkedRootSurvives)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();

    const char* SiblingPath = "/ReapSibling/Foo";
    const FAssetDataPtr Sibling = CreateAssetAt(Registry, SiblingPath);
    ASSERT_NE(Sibling, nullptr);
    const FGuid SiblingGuid = Sibling->AssetGUID;

    FAssetRegistryTestAccess::SetLastDiscovery(Registry, { FFixedString("/Reap") }, {});
    FAssetRegistryTestAccess::ReapStaleEntries(Registry);

    EXPECT_NE(Registry.GetAssetByPath(SiblingPath), nullptr) << "/ReapSibling is not nested under /Reap";

    Registry.AssetDeleted(SiblingGuid);
}

// An entry genuinely under a walked root that discovery did not visit has no file left on disk, so it is reaped.
TEST(AssetRegistryReap, AnEntryUnderAWalkedRootThatWasNotVisitedIsReaped)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();

    const char* MissingPath = "/Reap/Missing";
    const FAssetDataPtr Missing = CreateAssetAt(Registry, MissingPath);
    ASSERT_NE(Missing, nullptr);

    FAssetRegistryTestAccess::SetLastDiscovery(Registry, { FFixedString("/Reap") }, {});
    FAssetRegistryTestAccess::ReapStaleEntries(Registry);

    EXPECT_EQ(Registry.GetAssetByPath(MissingPath), nullptr);
}

// A lookup result held across a reap on another thread still reads the entry it was given, even after the main thread pumps.
TEST(AssetRegistryReap, AHeldEntryOutlivesAReapOnAnotherThread)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();

    const char* HeldPath = "/ReapHeld/Entry";
    const FAssetDataPtr Held = CreateAssetAt(Registry, HeldPath);
    ASSERT_NE(Held, nullptr);
    const FGuid HeldGuid = Held->AssetGUID;
    const FFixedString HeldFullPath = Held->Path;

    std::thread Reaper([&Registry]
    {
        FAssetRegistryTestAccess::SetLastDiscovery(Registry, { FFixedString("/ReapHeld") }, {});
        FAssetRegistryTestAccess::ReapStaleEntries(Registry);
    });
    Reaper.join();
    MainThread::ProcessQueue();

    // Fresh entries would land in the freed block if the reap had released what the caller holds.
    TVector<TSharedPtr<FAssetData>> Churn;
    for (int32 Index = 0; Index < 64; ++Index)
    {
        Churn.Add(MakeShared<FAssetData>(FAssetData{ .AssetGUID = FGuid::New(), .Path = FFixedString("/ReapHeld/Churn") }));
    }

    EXPECT_EQ(Registry.GetAssetByPath(HeldPath), nullptr);
    EXPECT_EQ(Held->AssetGUID, HeldGuid);
    EXPECT_EQ(Held->Path, HeldFullPath);
}

// A reap holds AssetsMutex while it dirties the reverse map, so a referencer query rebuilding that map must lock in the same order.
TEST(AssetRegistryReap, AReapRacingAReferencerQueryDoesNotDeadlock)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();

    const FGuid Target = FGuid::New();
    constexpr int32 Iterations = 2000;

    std::thread Reaper([&Registry, Target]
    {
        for (int32 Index = 0; Index < Iterations; ++Index)
        {
            FAssetRegistryTestAccess::ReplaceEntry(Registry, FAssetData{ .AssetGUID = FGuid::New(), .Path = FFixedString("/ReapRace/Referrer"), .Dependencies = { { .TargetGUID = Target } } });
            FAssetRegistryTestAccess::SetLastDiscovery(Registry, { FFixedString("/ReapRace") }, {});
            FAssetRegistryTestAccess::ReapStaleEntries(Registry);
        }
    });

    for (int32 Index = 0; Index < Iterations; ++Index)
    {
        Registry.GetReferencersOf(Target);
    }
    Reaper.join();

    EXPECT_TRUE(Registry.GetReferencersOf(Target).IsEmpty());
}
