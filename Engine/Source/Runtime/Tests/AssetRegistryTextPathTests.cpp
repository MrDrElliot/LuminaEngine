#include <gtest/gtest.h>

#include <chrono>

#include "AssetRegistryTestAccess.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/StringFormat.h"
#include "TaskSystem/ThreadedCallback.h"
#include <thread>

using namespace Lumina;

namespace
{
    FFixedString TextPath(const char* Folder, int32 Index)
    {
        return FormatAs<FFixedString>("{}/Doc_{}.rml", Folder, Index);
    }

    FStringView View(const FFixedString& Path) { return FStringView(*Path, Path.Len()); }

    void AddTextAssets(FAssetRegistry& Registry, const char* Folder, int32 Count)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FAssetRegistryTestAccess::AddTextAsset(Registry, View(TextPath(Folder, Index)));
        }
    }

    void RemoveTextAssets(FAssetRegistry& Registry, const char* Folder, int32 Count)
    {
        FScopedAssetRegistryBatch Batch;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Registry.TextAssetDeleted(View(TextPath(Folder, Index)));
        }
    }

    // Best of several runs, so a preempted run cannot fake a slow lookup.
    double NanosPerLookup(const FAssetRegistry& Registry, const FFixedString& Path)
    {
        constexpr int32 Lookups = 20000;
        constexpr int32 Runs = 5;
        double Best = 0.0;
        for (int32 Run = 0; Run < Runs; ++Run)
        {
            const auto Start = std::chrono::steady_clock::now();
            for (int32 Lookup = 0; Lookup < Lookups; ++Lookup)
            {
                EXPECT_NE(Registry.GetTextAssetByPath(View(Path)), nullptr);
            }
            const double Nanos = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - Start).count() / Lookups;
            Best = Run == 0 ? Nanos : std::min(Best, Nanos);
        }
        return Best;
    }
}

TEST(AssetRegistryTextPath, LookupCostDoesNotGrowWithTextAssetCount)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();
    constexpr int32 SmallCount = 256;
    constexpr int32 LargeCount = 8192;

    AddTextAssets(Registry, "/TextPathSmall", SmallCount);
    const FFixedString Probe = TextPath("/TextPathSmall", SmallCount / 2);
    const double SmallNanos = NanosPerLookup(Registry, Probe);

    AddTextAssets(Registry, "/TextPathLarge", LargeCount);
    const double LargeNanos = NanosPerLookup(Registry, Probe);

    // A scan over every entry would cost about 32 times more here.
    EXPECT_LT(LargeNanos, SmallNanos * 4.0) << "small " << SmallNanos << " ns, large " << LargeNanos << " ns";

    RemoveTextAssets(Registry, "/TextPathSmall", SmallCount);
    RemoveTextAssets(Registry, "/TextPathLarge", LargeCount);
}

TEST(AssetRegistryTextPath, IndexFollowsRenameAndDelete)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();
    FAssetRegistryTestAccess::AddTextAsset(Registry, "/TextPathEdit/Old.rml");

    const FTextAssetDataPtr Added = Registry.GetTextAssetByPath("/TextPathEdit/Old.rml");
    ASSERT_NE(Added, nullptr);
    EXPECT_EQ(Registry.GetTextAssetByPath("/TextPathEdit/Old"), Added);
    const FGuid Guid = Added->Guid;

    Registry.TextAssetRenamed("/TextPathEdit/Old.rml", "/TextPathEdit/New.rml");
    EXPECT_EQ(Registry.GetTextAssetByPath("/TextPathEdit/Old.rml"), nullptr);
    const FTextAssetDataPtr Renamed = Registry.GetTextAssetByPath("/TextPathEdit/New.rml");
    ASSERT_NE(Renamed, nullptr);
    EXPECT_EQ(Renamed->Guid, Guid);

    Registry.TextAssetFolderRenamed("/TextPathEdit", "/TextPathMoved");
    EXPECT_EQ(Registry.GetTextAssetByPath("/TextPathEdit/New.rml"), nullptr);
    ASSERT_NE(Registry.GetTextAssetByPath("/TextPathMoved/New.rml"), nullptr);

    Registry.TextAssetDeleted("/TextPathMoved/New.rml");
    EXPECT_EQ(Registry.GetTextAssetByPath("/TextPathMoved/New.rml"), nullptr);
    EXPECT_EQ(Registry.GetTextAssetByGUID(Guid), nullptr);
}

// A text lookup held across a rename and delete on another thread still reads the entry it was given.
TEST(AssetRegistryTextPath, AHeldEntryOutlivesARenameOnAnotherThread)
{
    FAssetRegistry& Registry = FAssetRegistry::Get();
    const FFixedString OldPath("/TextPathHeld/Old.rml");
    const FFixedString NewPath("/TextPathHeld/New.rml");
    FAssetRegistryTestAccess::AddTextAsset(Registry, View(OldPath));

    const FTextAssetDataPtr Held = Registry.GetTextAssetByPath(View(OldPath));
    ASSERT_NE(Held, nullptr);
    const FGuid Guid = Held->Guid;

    std::thread Renamer([&]
    {
        Registry.TextAssetRenamed(View(OldPath), View(NewPath));
    });
    Renamer.join();
    MainThread::ProcessQueue();

    EXPECT_EQ(Held->Path, OldPath);
    const FTextAssetDataPtr Current = Registry.GetTextAssetByGUID(Guid);
    ASSERT_NE(Current, nullptr);
    EXPECT_NE(Current, Held);
    EXPECT_EQ(Current->Path, NewPath);
    EXPECT_EQ(Registry.GetTextAssetByPath(View(NewPath)), Current);
    EXPECT_EQ(Registry.GetTextAssetByPath(View(OldPath)), nullptr);

    std::thread Deleter([&]
    {
        Registry.TextAssetDeleted(View(NewPath));
    });
    Deleter.join();
    MainThread::ProcessQueue();

    EXPECT_EQ(Registry.GetTextAssetByGUID(Guid), nullptr);
    EXPECT_EQ(Current->Path, NewPath);
    EXPECT_EQ(Current->Guid, Guid);
}
