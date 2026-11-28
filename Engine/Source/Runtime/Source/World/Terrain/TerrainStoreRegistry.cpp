#include "RuntimePCH.h"
#include "TerrainStoreRegistry.h"
#include "FileSystem/FileSystem.h"
#include "GUID/GUID.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "TerrainStore.h"

namespace Lumina
{
    FTerrainStoreRegistry::FTerrainStoreRegistry() = default;
    FTerrainStoreRegistry::~FTerrainStoreRegistry() = default;

    TSharedPtr<FTerrainStore> FTerrainStoreRegistry::Acquire(FStringView DataFile)
    {
        if (DataFile.IsEmpty())
        {
            return nullptr;
        }
        const FString Key(DataFile);
        TScopeLock Guard(Lock);
        if (TSharedPtr<FTerrainStore>* Found = Stores.Find(Key))
        {
            return *Found;
        }
        const FString DiskPath = ResolveDiskPath(DataFile);
        if (DiskPath.IsEmpty() || !Filesystem::IsFile(DiskPath))
        {
            return nullptr;
        }
        TSharedPtr<FTerrainStore> Store = FTerrainStore::Open(DiskPath);
        if (Store != nullptr)
        {
            Stores.Add(Key, Store);
        }
        return Store;
    }

    void FTerrainStoreRegistry::Register(FStringView DataFile, const TSharedPtr<FTerrainStore>& Store)
    {
        if (DataFile.IsEmpty())
        {
            return;
        }
        TScopeLock Guard(Lock);
        Stores.FindOrAdd(FString(DataFile)) = Store;
    }

    void FTerrainStoreRegistry::Collect()
    {
        // Trimming walks a store's cache, so the check runs about once a second of frames.
        constexpr uint32 kCollectInterval = 64u;
        if (++CollectCalls % kCollectInterval != 0u)
        {
            return;
        }

        TScopeLock Guard(Lock);
        TVector<FString> Released;
        for (auto& [DataFile, Store] : Stores)
        {
            if (!Store.IsUnique())
            {
                continue;
            }
            if (Store->HasFile() && !Store->IsDirty())
            {
                Released.Add(DataFile);
            }
            else
            {
                Store->ReleaseCleanTiles();
            }
        }
        for (const FString& DataFile : Released)
        {
            Stores.Remove(DataFile);
        }
    }

    FString FTerrainStoreRegistry::MakeDataFile()
    {
        return FString("/Game/Content/Terrain/") + FGuid::New().ToString(true, false) + ".lterrain";
    }

    FString FTerrainStoreRegistry::ResolveDiskPath(FStringView DataFile)
    {
        const FPathString Resolved = VFS::ResolvePath(DataFile);
        return FString(Resolved.View());
    }
}
