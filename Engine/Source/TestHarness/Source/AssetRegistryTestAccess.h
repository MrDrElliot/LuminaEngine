#pragma once

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/Algorithm.h"

namespace Lumina
{
    // What a test needs from the registry's discovery internals to drive a reap without a real filesystem walk.
    struct FAssetRegistryTestAccess
    {
        static void SetLastDiscovery(FAssetRegistry& Registry, TVector<FFixedString> WalkedRoots, TVector<FFixedString> VisitedPaths)
        {
            Algo::Sort(VisitedPaths);
            Registry.LastDiscoveryWalkedRoots = Move(WalkedRoots);
            Registry.LastDiscoveryVisitedPaths = Move(VisitedPaths);
        }

        static size_t ReapStaleEntries(FAssetRegistry& Registry) { return Registry.ReapStaleEntries(); }

        // Lists Data in place of the entry sharing its GUID, as a rescan of the edited file would.
        static void ReplaceEntry(FAssetRegistry& Registry, FAssetData Data)
        {
            FWriteScopeLock Lock(Registry.AssetsMutex);
            Registry.InsertAssetLocked(MakeShared<FAssetData>(Move(Data)));
        }

        // Lists a text asset without minting a sidecar on disk.
        static void AddTextAsset(FAssetRegistry& Registry, FStringView Path)
        {
            auto Data = MakeShared<FTextAssetData>();
            Data->Guid = FGuid::New();
            Data->Path = Path;
            Data->Kind = TextAsset::KindFromPath(Path);

            FWriteScopeLock Lock(Registry.TextAssetsMutex);
            Registry.AddTextAssetLocked(Move(Data));
        }
    };
}
