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

    // Instantiate runs at the sync point, since it creates entities and fires construct hooks, so only the timers are declared.
    void SPrefabSpawnerSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart, EUpdatePriority::Low);
        Writes<SPrefabSpawnerComponent>();
        Reads<STransformComponent>();
    }

    void SPrefabSpawnerSystem::OnStartup()
    {
        const FSystemContext& Context = GetContext();

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

    void SPrefabSpawnerSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        const float DeltaTime = static_cast<float>(Context.GetDeltaTime());

        CWorld* World = Context.GetWorld();
        ECS::FCommandBus& Bus = Context.GetCommandBus();
        auto View = Context.CreateView<SPrefabSpawnerComponent, STransformComponent>();
        for (ECS::FEntity Entity : View)
        {
            SPrefabSpawnerComponent* SpawnComponent = &View.Get<SPrefabSpawnerComponent>(Entity);
            const STransformComponent* Transform = &View.Get<STransformComponent>(Entity);

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

            const float Remainder = SpawnComponent->TimeUntilNextSpawn;
            SpawnComponent->TimeUntilNextSpawn = Math::Max(DrawSpawnInterval(SpawnComponent->SpawnTimeRange) + Remainder, 0.0001f);

            Bus.Enqueue([World, Entity, At = Transform->GetWorldTransform()](ECS::FRegistry& Registry)
            {
                if (const SPrefabSpawnerComponent* Live = Registry.IsValid(Entity) ? Registry.TryGet<SPrefabSpawnerComponent>(Entity) : nullptr)
                {
                    Live->Spawn(World, At);
                }
            });
        }
    }

    void SPrefabSpawnerSystem::OnTeardown()
    {
    }
}
