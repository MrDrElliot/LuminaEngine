#include "RuntimePCH.h"
#include "TerrainComponent.h"
#include "Core/Engine/Engine.h"
#include "World/Terrain/TerrainStoreRegistry.h"

namespace Lumina
{
    namespace
    {
        FTerrainStoreRegistry* Registry()
        {
            return Global::GEngine != nullptr ? &Global::GEngine->GetTerrainStores() : nullptr;
        }
    }

    FTerrainStore* STerrainComponent::EnsureStore()
    {
        if (Store != nullptr)
        {
            return Store.Get();
        }

        FTerrainStoreRegistry* Stores = Registry();
        if (Stores != nullptr && !DataFile.IsEmpty())
        {
            Store = Stores->Acquire(DataFile);
        }
        if (Store == nullptr)
        {
            const int64 Samples = int64(Resolution) * Resolution;
            if (Resolution >= 2 && (int64)Heightmap.Num() == Samples)
            {
                Store = FTerrainStore::CreateFromArrays(Resolution, Heightmap, LayerWeights, Layers.Num());
            }
            else
            {
                if (!DataFile.IsEmpty())
                {
                    LOG_WARN("Terrain data {} is missing, so the terrain starts flat.", DataFile);
                }
                Store = FTerrainStore::CreateFlat(Math::Max(Resolution, 2), 0.0f);
            }
            if (DataFile.IsEmpty())
            {
                DataFile = FTerrainStoreRegistry::MakeDataFile();
            }
            if (Stores != nullptr)
            {
                Stores->Register(DataFile, Store);
            }
        }
        Heightmap.Reset();
        LayerWeights.Reset();
        Resolution = Store->GetResolution();
        MarkHeightmapReplaced();
        CPUState.bFullWeightsDirty = true;
        return Store.Get();
    }

    void STerrainComponent::SetStore(TSharedPtr<FTerrainStore> NewStore, FStringView InDataFile)
    {
        Store      = Move(NewStore);
        DataFile   = InDataFile.IsEmpty() ? FTerrainStoreRegistry::MakeDataFile() : FString(InDataFile);
        Resolution = Store->GetResolution();
        Heightmap.Reset();
        LayerWeights.Reset();
        if (FTerrainStoreRegistry* Stores = Registry())
        {
            Stores->Register(DataFile, Store);
        }
        MarkHeightmapReplaced();
        CPUState.bFullWeightsDirty = true;
    }

    bool STerrainComponent::SaveStore() const
    {
        if (Store == nullptr || (Store->HasFile() && !Store->IsDirty()))
        {
            return true;
        }
        const FString DiskPath = FTerrainStoreRegistry::ResolveDiskPath(DataFile);
        if (DiskPath.IsEmpty())
        {
            LOG_ERROR("Terrain data {} maps to no folder, so its edits are not saved.", DataFile);
            return false;
        }
        return Store->Save(DiskPath);
    }
}
