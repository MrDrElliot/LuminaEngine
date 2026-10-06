#include "RuntimePCH.h"

#include "PrefabLibrary.h"

#include "Assets/AssetRef.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Log/Log.h"
#include "World/World.h"

namespace Lumina
{
    ECS::FEntity CPrefabLibrary::SpawnPrefab(CWorld* World, const FAssetRef& Prefab)
    {
        return SpawnPrefabAt(World, Prefab, FTransform(), ECS::NullEntity);
    }

    ECS::FEntity CPrefabLibrary::SpawnPrefabAt(CWorld* World, const FAssetRef& Prefab,
        const FTransform& SpawnTransform, ECS::FEntity Parent)
    {
        if (World == nullptr)
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

        return PrefabObject->Instantiate(World, SpawnTransform, Parent);
    }

    void CPrefabLibrary::SpawnPrefabAsync(CWorld* World, const FAssetRef& Prefab, FScriptCallback OnSpawned)
    {
        if (World == nullptr)
        {
            Scripting::InvokeScriptCallback(OnSpawned, (uint64)(uint32)ECS::NullEntity);
            return;
        }

        const FName Path(Prefab.GetPath());
        TStrongObjectPtr<CWorld> WorldHandle(World);
        AsyncLoadObject(Path, [WorldHandle, Path, OnSpawned](CObject* Object)
        {
            CWorld* Target = WorldHandle.Get();
            CPrefab* Loaded = Cast<CPrefab>(Object);
            if (Target == nullptr || Loaded == nullptr)
            {
                if (Loaded == nullptr)
                {
                    LOG_WARN("SpawnPrefabAsync: asset '{}' is not a CPrefab", Path.c_str());
                }
                Scripting::InvokeScriptCallback(OnSpawned, (uint64)(uint32)ECS::NullEntity);
                return;
            }

            const ECS::FEntity Spawned = Loaded->Instantiate(Target, FTransform(), ECS::NullEntity);
            Scripting::InvokeScriptCallback(OnSpawned, (uint64)(uint32)Spawned);
        });
    }
}
