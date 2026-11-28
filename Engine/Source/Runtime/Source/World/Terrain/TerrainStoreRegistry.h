#pragma once

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Core/Threading/Sync.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FTerrainStore;

    // The terrain stores open by data file, so a component rebuilt by undo, PIE or a reload finds the same edits.
    class RUNTIME_API FTerrainStoreRegistry
    {
    public:

        FTerrainStoreRegistry();
        ~FTerrainStoreRegistry();

        // The registered store, else the file opened from disk, else null.
        TSharedPtr<FTerrainStore> Acquire(FStringView DataFile);
        void Register(FStringView DataFile, const TSharedPtr<FTerrainStore>& Store);

        // Frees stores nothing else holds once the disk has all their data, and trims the cached tiles of those it lacks. Cheap every frame.
        void Collect();

        // A fresh data file under the project content, which the world saves the store to.
        static FString MakeDataFile();
        // The disk path of a data file, empty when no mount maps it.
        static FString ResolveDiskPath(FStringView DataFile);

    private:

        // A deleted terrain's undo may still want its edits, so a store stays here until the disk has them.
        FMutex Lock;
        THashMap<FString, TSharedPtr<FTerrainStore>> Stores;
        uint32 CollectCalls = 0;
    };
}
