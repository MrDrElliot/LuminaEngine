#include "RuntimePCH.h"

#include "World/Subsystems/SpawnSubsystem.h"

#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Log/Log.h"
#include "World/World.h"

namespace Lumina
{
    ECS::FEntity CSpawnSubsystem::SpawnPrefab(const TSoftObjectPtr<CPrefab>& Prefab)
    {
        CWorld* World = GetWorld();
        return SpawnPrefabAt(Prefab, FTransform(), ECS::NullEntity);
    }

    ECS::FEntity CSpawnSubsystem::SpawnPrefabAt(const TSoftObjectPtr<CPrefab>& Prefab,
        const FTransform& SpawnTransform, ECS::FEntity Parent)
    {
        if (GetWorld() == nullptr)
        {
            return ECS::NullEntity;
        }

        FStringView Path = Prefab.GetPath();
        FAssetData* AssetData = FAssetRegistry::Get().GetAssetByPath(Path);
        if (AssetData == nullptr)
        {
            LOG_WARN("SpawnPrefab: no asset found at path '{}'", Path);
            return ECS::NullEntity;
        }

        // A cold spawn fans the prefab's closure across the swarm; a resident one takes the lookup.
        CPrefab* PrefabObject = FindObject<CPrefab>(AssetData->AssetGUID);
        if (PrefabObject == nullptr)
        {
            PrefabObject = LoadObjectGraph<CPrefab>(AssetData->AssetGUID);
        }
        if (PrefabObject == nullptr)
        {
            LOG_WARN("SpawnPrefab: asset '{}' is not a CPrefab", Path);
            return ECS::NullEntity;
        }

        return SpawnLoadedPrefabAt(PrefabObject, SpawnTransform, Parent);
    }

    ECS::FEntity CSpawnSubsystem::SpawnLoadedPrefab(CPrefab* Prefab)
    {
        CWorld* World = GetWorld();
        return SpawnLoadedPrefabAt(Prefab, FTransform(), ECS::NullEntity);
    }

    ECS::FEntity CSpawnSubsystem::SpawnLoadedPrefabAt(CPrefab* Prefab, const FTransform& SpawnTransform, ECS::FEntity Parent)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || Prefab == nullptr)
        {
            return ECS::NullEntity;
        }
        return Prefab->Instantiate(World, SpawnTransform, Parent);
    }

    void CSpawnSubsystem::SpawnPrefabAsync(const TSoftObjectPtr<CPrefab>& Prefab, SScriptCallback OnSpawned)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            Scripting::InvokeScriptCallback(OnSpawned, (uint64)(uint32)ECS::NullEntity);
            return;
        }

        const FName Path(Prefab.GetPath());
        TStrongObjectPtr<CWorld> WorldHandle(World);
        FAssetManager::Get().Load(Prefab.GetPath()).OnReady([WorldHandle, Path, OnSpawned](const FAssetHandle& Handle)
        {
            CWorld* Target = WorldHandle.Get();
            CPrefab* Loaded = Handle.Get<CPrefab>();
            if (Target == nullptr || Loaded == nullptr)
            {
                if (Loaded == nullptr)
                {
                    LOG_WARN("SpawnPrefabAsync: asset '{}' is not a CPrefab", *Path);
                }
                Scripting::InvokeScriptCallback(OnSpawned, (uint64)(uint32)ECS::NullEntity);
                return;
            }

            const ECS::FEntity Spawned = Target->GetSpawner()->SpawnLoadedPrefab(Loaded);
            Scripting::InvokeScriptCallback(OnSpawned, (uint64)(uint32)Spawned);
        });
    }
}
