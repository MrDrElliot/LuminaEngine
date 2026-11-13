#include <gtest/gtest.h>

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Components/PrefabSpawnerComponent.h"
#include "Core/Object/ObjectCore.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    // A bare game world running only CPrefabSpawnerSystem, with a fixed interval so every draw is deterministic.
    struct FSpawnerWorld
    {
        TStrongObjectPtr<CWorld> World;
        ECS::FEntity             Entity;

        FSpawnerWorld()
        {
            RegisterPendingTypes();
            World = TStrongObjectPtr<CWorld>(NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient));
            FWorldTestAccess::SetWorldType(*World, EWorldType::Game);
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            Registry.Ctx().Emplace<CWorld*>(World.Get());

            FWorldTestAccess::KeepOnlySystems(*World, { "CPrefabSpawnerSystem" });

            Entity = Registry.Create();
            Registry.Emplace<STransformComponent>(Entity);
            Registry.Emplace<SPrefabSpawnerComponent>(Entity).SpawnTimeRange = FVector2{1.0f, 1.0f};
        }

        SPrefabSpawnerComponent& Spawner() const { return World->GetEntityRegistry().Get<SPrefabSpawnerComponent>(Entity); }

        float Tick(double DeltaSeconds = 0.75) const
        {
            FWorldTestAccess::TickStage(*World, EUpdateStage::FrameStart, DeltaSeconds);
            return Spawner().TimeUntilNextSpawn;
        }
    };

    // The spawner points at a resident one-entity prefab by GUID, so a startup spawn instantiates it synchronously.
    struct FStartupSpawnerWorld : FSpawnerWorld
    {
        TStrongObjectPtr<CPrefab> Prefab{ NewObject<CPrefab>(nullptr, NAME_None, FGuid::New(), OF_Transient) };

        FStartupSpawnerWorld()
        {
            const ECS::FEntity Root = Prefab->Registry.Create();
            Prefab->Registry.Emplace<SPrefabComponent>(Root).StableID = FName("root");
            Prefab->Registry.Emplace<STransformComponent>(Root);

            Spawner().PrefabInstance = TSoftObjectPtr<CPrefab>(Prefab->GetGUID());
            Spawner().bSpawnOnStartup = true;
        }

        int32 EntitiesAddedByStartup() const
        {
            const ECS::FRegistry& Registry = World->GetEntityRegistry();
            const int32 Before = Registry.NumEntities();
            World->Systems[0]->OnStartup(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
            return Registry.NumEntities() - Before;
        }
    };
}

TEST(PrefabSpawnerSystem, ArmsTheTimerOnFirstSightInsteadOfSpawning)
{
    FSpawnerWorld Fixture;
    ASSERT_EQ(Fixture.World->Systems.Num(), 1) << "CPrefabSpawnerSystem was not found";
    EXPECT_FLOAT_EQ(Fixture.Spawner().TimeUntilNextSpawn, 0.0f);

    EXPECT_FLOAT_EQ(Fixture.Tick(), 1.0f) << "the first update draws a full interval without counting down";
    EXPECT_FALSE(Fixture.World->GetCommandBus().HasPending());
    EXPECT_FLOAT_EQ(Fixture.Tick(), 0.25f);
}

TEST(PrefabSpawnerSystem, CarriesTheOvershootIntoTheNextInterval)
{
    FSpawnerWorld Fixture;

    Fixture.Tick();
    EXPECT_FLOAT_EQ(Fixture.Tick(), 0.25f);
    EXPECT_FLOAT_EQ(Fixture.Tick(), 0.5f) << "the 0.5s overshoot is taken off the freshly drawn interval";
}

TEST(PrefabSpawnerSystem, ANonPositiveMinimumDisablesSpawningUntilReEnabled)
{
    FSpawnerWorld Fixture;
    Fixture.Tick();
    EXPECT_FLOAT_EQ(Fixture.Tick(), 0.25f);

    Fixture.Spawner().SpawnTimeRange = FVector2{-1.0f, 5.0f};
    EXPECT_FLOAT_EQ(Fixture.Tick(), 0.0f) << "disabling disarms a running timer";
    EXPECT_FLOAT_EQ(Fixture.Tick(5.0), 0.0f);

    Fixture.Spawner().SpawnTimeRange = FVector2{1.0f, 1.0f};
    EXPECT_FLOAT_EQ(Fixture.Tick(), 1.0f) << "re-enabling arms a full interval again";
}

TEST(PrefabSpawnerSystem, ADeferredSpawnSkipsAnEntityDestroyedBeforeItRuns)
{
    FSpawnerWorld Fixture;
    ECS::FRegistry& Registry = Fixture.World->GetEntityRegistry();
    Fixture.Tick(1.0);

    Fixture.World->Systems[0]->OnUpdate(FWorldTestAccess::SystemContext(*Fixture.World));
    ASSERT_TRUE(Fixture.World->GetCommandBus().HasPending()) << "the expired timer should have deferred a spawn";

    Registry.Destroy(Fixture.Entity);
    const ECS::FEntity Recycled = Registry.Create();
    Registry.Emplace<SPrefabSpawnerComponent>(Recycled);

    FWorldTestAccess::TickStage(*Fixture.World, EUpdateStage::FrameStart, 0.0);
    EXPECT_FALSE(Fixture.World->GetCommandBus().HasPending());
    EXPECT_FALSE(Registry.IsValid(Fixture.Entity));
    EXPECT_TRUE(Registry.IsValid(Recycled));
}

TEST(PrefabSpawnerSystem, StartupSpawnsEveryFlaggedSpawner)
{
    FStartupSpawnerWorld Fixture;
    ECS::FRegistry& Registry = Fixture.World->GetEntityRegistry();
    const SPrefabSpawnerComponent SameSpawner = Fixture.Spawner();
    const ECS::FEntity Second = Registry.Create();
    Registry.Emplace<STransformComponent>(Second);
    Registry.Emplace<SPrefabSpawnerComponent>(Second, SameSpawner);

    EXPECT_EQ(Fixture.EntitiesAddedByStartup(), 2) << "the first spawn adds components, and the second spawner must still be found";
}

TEST(PrefabSpawnerSystem, StartupSkipsASpawnerNotFlaggedForStartup)
{
    FStartupSpawnerWorld Fixture;
    Fixture.Spawner().bSpawnOnStartup = false;

    EXPECT_EQ(Fixture.EntitiesAddedByStartup(), 0);
}

TEST(PrefabSpawnerSystem, StartupSkipsASpawnerWithoutATransform)
{
    FStartupSpawnerWorld Fixture;
    Fixture.World->GetEntityRegistry().Remove<STransformComponent>(Fixture.Entity);

    EXPECT_EQ(Fixture.EntitiesAddedByStartup(), 0);
}

TEST(PrefabSpawnerSystem, StartupDoesNothingInAnEditorWorld)
{
    FStartupSpawnerWorld Fixture;
    FWorldTestAccess::SetWorldType(*Fixture.World, EWorldType::Editor);

    EXPECT_EQ(Fixture.EntitiesAddedByStartup(), 0);
}
