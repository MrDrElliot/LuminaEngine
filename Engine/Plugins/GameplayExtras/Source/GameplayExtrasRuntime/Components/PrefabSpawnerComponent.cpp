#include "PrefabSpawnerComponent.h"
#include "World/ECS/Registry.h"
#include "World/World.h"

// Prefab.h names ECS::FEntity without including the ECS facade itself, so it has to come first.
#include "Assets/AssetTypes/Prefabs/Prefab.h"

namespace Lumina
{
    bool SPrefabSpawnerComponent::Spawn(CWorld* World, const FTransform& Transform) const
    {
        if (World == nullptr || PrefabInstance.IsNull())
        {
            return false;
        }

        PrefabInstance.LoadAsync([WeakWorld = TWeakObjectPtr<CWorld>(World), Transform](CPrefab* Prefab)
        {
            CWorld* Target = WeakWorld.Get();
            if (Prefab == nullptr || Target == nullptr || Target->IsTearingDown())
            {
                return;
            }

            Prefab->Instantiate(Target, Transform);
        });

        return true;
    }
}
