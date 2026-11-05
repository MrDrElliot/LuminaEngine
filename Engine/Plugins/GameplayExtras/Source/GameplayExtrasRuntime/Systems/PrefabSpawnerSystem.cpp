#include "PrefabSpawnerSystem.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "World/ECS/Registry.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Components/PrefabSpawnerComponent.h"
#include "Core/Math/Math.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    namespace PrefabSpawnerSystemInternal
    {
        float DrawSpawnInterval(const FVector2& Range)
        {
            // Clamped at zero so an interval can never land on 0 and spawn every single frame.
            const float Min = Math::Max(Math::Min(Range.x, Range.y), 0.0f);
            const float Max = Math::Max(Range.x, Range.y);

            return Math::RandRange(Min, Max);
        }
    }

    // Instantiate runs deferred to the main thread, since it creates entities and fires construct hooks, so only the timers are touched here.
    void SPrefabSpawnerSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
    }

    void SPrefabSpawnerSystem::OnStartup(const SExclusiveContext& Context)
    {
        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }

        // Collected first, since Spawn creates entities and components that can reallocate a live walk.
        for (ECS::FEntity Entity : Spawners.Collect())
        {
            if (!Context.IsValidEntity(Entity))
            {
                continue;
            }

            // Re-resolved per entity, since an earlier spawn may have moved or removed these components.
            const SPrefabSpawnerComponent* SpawnComponent = Spawners.TryGet<SPrefabSpawnerComponent>(Entity);
            const STransformComponent* Transform = Spawners.TryGet<STransformComponent>(Entity);
            if (SpawnComponent == nullptr || Transform == nullptr || !SpawnComponent->bSpawnOnStartup)
            {
                continue;
            }

            SpawnComponent->Spawn(Context.GetWorld(), Transform->GetWorldTransform());
        }
    }

    void SPrefabSpawnerSystem::OnUpdate(const SSystemContext& Context)
    {
        const float DeltaTime = Context.GetDeltaTime();

        Spawners.ForEach([&](ECS::FEntity Entity, SPrefabSpawnerComponent& SpawnComponent, const STransformComponent& Transform)
        {
            if (Math::Max(SpawnComponent.SpawnTimeRange.x, SpawnComponent.SpawnTimeRange.y) <= 0.0f)
            {
                SpawnComponent.TimeUntilNextSpawn = 0.0f;
                return;
            }

            if (SpawnComponent.TimeUntilNextSpawn <= 0.0f)
            {
                SpawnComponent.TimeUntilNextSpawn = PrefabSpawnerSystemInternal::DrawSpawnInterval(SpawnComponent.SpawnTimeRange);
                return;
            }

            SpawnComponent.TimeUntilNextSpawn -= DeltaTime;
            if (SpawnComponent.TimeUntilNextSpawn > 0.0f)
            {
                return;
            }

            const float Remainder = SpawnComponent.TimeUntilNextSpawn;
            SpawnComponent.TimeUntilNextSpawn = Math::Max(PrefabSpawnerSystemInternal::DrawSpawnInterval(SpawnComponent.SpawnTimeRange) + Remainder, 0.0001f);

            Context.Defer([Entity, At = Transform.GetWorldTransform()](const SExclusiveContext& X)
            {
                ECS::FRegistry& Registry = X.GetRegistry();
                if (const SPrefabSpawnerComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<ECS::Read<SPrefabSpawnerComponent>>(Entity) : nullptr)
                {
                    Live->Spawn(X.GetWorld(), At);
                }
            });
        });
    }
}
