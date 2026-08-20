#include "PrefabSpawnerSystem.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Components/PrefabSpawnerComponent.h"
#include "Core/Math/Math.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    namespace
    {
        float DrawSpawnInterval(const FVector2& Range)
        {
            // Clamped at zero so an interval can never land on 0 and spawn every single frame.
            const float Min = Math::Max(Math::Min(Range.x, Range.y), 0.0f);
            const float Max = Math::Max(Range.x, Range.y);

            return Math::RandRange(Min, Max);
        }

        // Instantiate creates entities and every component the prefab carries, so a live view over the
        // spawner's own storages can be reallocated out from under the loop.
        void GatherSpawners(const FSystemContext& Context, TVector<entt::entity>& Out)
        {
            auto View = Context.CreateView<SPrefabSpawnerComponent, STransformComponent>();
            Out.reserve(View.size_hint());
            for (entt::entity Entity : View)
            {
                Out.push_back(Entity);
            }
        }
    }

    // Exclusive: Instantiate does structural ECS changes, creates physics bodies, and fires entt construct
    // hooks for whatever the prefab contains. No component-level declaration can describe that.
    FSystemAccess SPrefabSpawnerSystem::Access = FSystemAccess::Exclusive();

    void SPrefabSpawnerSystem::Startup(const FSystemContext& Context) noexcept
    {
        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }

        FEntityRegistry& Registry = Context.GetRegistry();

        TVector<entt::entity> Spawners;
        GatherSpawners(Context, Spawners);

        for (entt::entity Entity : Spawners)
        {
            if (!Registry.valid(Entity))
            {
                continue;
            }

            // Re-resolved per entity: an earlier spawn may have moved or removed these components.
            const SPrefabSpawnerComponent* SpawnComponent = Registry.try_get<SPrefabSpawnerComponent>(Entity);
            const STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
            if (SpawnComponent == nullptr || Transform == nullptr || !SpawnComponent->bSpawnOnStartup)
            {
                continue;
            }

            SpawnComponent->Spawn(Context.GetWorld(), Transform->GetWorldTransform());
        }
    }

    void SPrefabSpawnerSystem::Update(const FSystemContext& Context) noexcept
    {
        const float DeltaTime = static_cast<float>(Context.GetDeltaTime());

        FEntityRegistry& Registry = Context.GetRegistry();

        TVector<entt::entity> Spawners;
        GatherSpawners(Context, Spawners);

        for (entt::entity Entity : Spawners)
        {
            if (!Registry.valid(Entity))
            {
                continue;
            }

            SPrefabSpawnerComponent* SpawnComponent = Registry.try_get<SPrefabSpawnerComponent>(Entity);
            const STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
            if (SpawnComponent == nullptr || Transform == nullptr)
            {
                continue;
            }

            if (Math::Max(SpawnComponent->SpawnTimeRange.x, SpawnComponent->SpawnTimeRange.y) <= 0.0f)
            {
                SpawnComponent->TimeUntilNextSpawn = 0.0f;
                continue;
            }

            if (SpawnComponent->TimeUntilNextSpawn <= 0.0f)
            {
                SpawnComponent->TimeUntilNextSpawn = DrawSpawnInterval(SpawnComponent->SpawnTimeRange);
                continue;
            }

            SpawnComponent->TimeUntilNextSpawn -= DeltaTime;
            if (SpawnComponent->TimeUntilNextSpawn > 0.0f)
            {
                continue;
            }

            const FVector2 Range = SpawnComponent->SpawnTimeRange;
            const float Remainder = SpawnComponent->TimeUntilNextSpawn;

            SpawnComponent->Spawn(Context.GetWorld(), Transform->GetWorldTransform());

            // Re-resolved: the spawn may have reallocated the spawner pool.
            SpawnComponent = Registry.try_get<SPrefabSpawnerComponent>(Entity);
            if (SpawnComponent != nullptr)
            {
                SpawnComponent->TimeUntilNextSpawn = Math::Max(DrawSpawnInterval(Range) + Remainder, 0.0001f);
            }
        }
    }

    void SPrefabSpawnerSystem::Teardown(const FSystemContext& Context) noexcept
    {
    }
}
