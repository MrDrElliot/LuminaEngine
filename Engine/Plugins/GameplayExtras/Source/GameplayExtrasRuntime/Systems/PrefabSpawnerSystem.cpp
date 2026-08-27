#include "PrefabSpawnerSystem.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "World/ECS/Registry.h"
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

        // Instantiate creates entities and components, so a live view can be reallocated under the loop.
        void GatherSpawners(const FSystemContext& Context, TVector<ECS::FEntity>& Out)
        {
            auto View = Context.CreateView<SPrefabSpawnerComponent, STransformComponent>();
            Out.reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Out.push_back(Entity);
            }
        }
    }

    // Instantiate does structural changes and fires construct hooks no declaration can describe.
    FSystemAccess SPrefabSpawnerSystem::Access = FSystemAccess::Exclusive();

    void SPrefabSpawnerSystem::Startup(const FSystemContext& Context) noexcept
    {
        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }

        ECS::FRegistry& Registry = Context.GetRegistry();

        TVector<ECS::FEntity> Spawners;
        GatherSpawners(Context, Spawners);

        for (ECS::FEntity Entity : Spawners)
        {
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            // Re-resolved per entity, since an earlier spawn may have moved or removed these components.
            const SPrefabSpawnerComponent* SpawnComponent = Registry.TryGet<SPrefabSpawnerComponent>(Entity);
            const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
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

        ECS::FRegistry& Registry = Context.GetRegistry();

        TVector<ECS::FEntity> Spawners;
        GatherSpawners(Context, Spawners);

        for (ECS::FEntity Entity : Spawners)
        {
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            SPrefabSpawnerComponent* SpawnComponent = Registry.TryGet<SPrefabSpawnerComponent>(Entity);
            const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
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

            // Re-resolved, since the spawn may have reallocated the spawner pool.
            SpawnComponent = Registry.TryGet<SPrefabSpawnerComponent>(Entity);
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
