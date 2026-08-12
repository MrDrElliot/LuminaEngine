#include "PrefabSpawnerComponent.h"

// Prefab.h names entt::entity without including the ECS facade itself, so it has to come first.
#include "World/Entity/EntityHandle.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"

namespace Lumina
{
    bool SPrefabSpawnerComponent::Spawn(CWorld* World, const FTransform& Transform) const
    {
        if (World == nullptr)
        {
            return false;
        }

        // @TODO Temp load sync, mirroring SPrefabSpawnerSystem::Startup.
        TObjectPtr<CPrefab> Prefab = PrefabInstance.LoadSynchronous();
        if (!Prefab.IsValid())
        {
            return false;
        }

        return Prefab->Instantiate(World, Transform) != entt::null;
    }
}
