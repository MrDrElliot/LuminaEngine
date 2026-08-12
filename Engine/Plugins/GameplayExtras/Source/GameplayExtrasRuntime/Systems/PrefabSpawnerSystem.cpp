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
    }

    FSystemAccess SPrefabSpawnerSystem::Access = FSystemAccess{}
        .Write<SPrefabComponent, SPrefabSpawnerComponent, STransformComponent>();

    void SPrefabSpawnerSystem::Startup(const FSystemContext& Context) noexcept
    {
        if (Context.GetWorldType() == EWorldType::Editor)
        {
            return;
        }
        
        auto View = Context.CreateView<SPrefabSpawnerComponent, STransformComponent>();
        View.each([&](SPrefabSpawnerComponent& SpawnComponent, const STransformComponent& Transform)
        {
            if (!SpawnComponent.bSpawnOnStartup)
            {
                return;
            }

            SpawnComponent.Spawn(Context.GetWorld(), Transform.GetWorldTransform());
        });
    }

    void SPrefabSpawnerSystem::Update(const FSystemContext& Context) noexcept
    {
        const float DeltaTime = static_cast<float>(Context.GetDeltaTime());

        auto View = Context.CreateView<SPrefabSpawnerComponent, STransformComponent>();
        View.each([&](SPrefabSpawnerComponent& SpawnComponent, const STransformComponent& Transform)
        {
            if (Math::Max(SpawnComponent.SpawnTimeRange.x, SpawnComponent.SpawnTimeRange.y) <= 0.0f)
            {
                SpawnComponent.TimeUntilNextSpawn = 0.0f;
                return;
            }

            if (SpawnComponent.TimeUntilNextSpawn <= 0.0f)
            {
                SpawnComponent.TimeUntilNextSpawn = DrawSpawnInterval(SpawnComponent.SpawnTimeRange);
                return;
            }

            SpawnComponent.TimeUntilNextSpawn -= DeltaTime;
            if (SpawnComponent.TimeUntilNextSpawn > 0.0f)
            {
                return;
            }

            SpawnComponent.Spawn(Context.GetWorld(), Transform.GetWorldTransform());
            
            SpawnComponent.TimeUntilNextSpawn = Math::Max(
                DrawSpawnInterval(SpawnComponent.SpawnTimeRange) + SpawnComponent.TimeUntilNextSpawn, 0.0001f);
        });
    }

    void SPrefabSpawnerSystem::Teardown(const FSystemContext& Context) noexcept
    {
    }
}
