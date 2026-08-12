#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Transform.h"
#include "Core/Object/SoftObjectPtr.h"
#include "PrefabSpawnerComponent.generated.h"

namespace Lumina
{
    class CPrefab;
    class CWorld;


    REFLECT(Component, Category = "Gameplay")
    struct GAMEPLAYEXTRASRUNTIME_API SPrefabSpawnerComponent
    {
        GENERATED_BODY()

        /**
         * Instantiates the prefab at Transform. Returns false if no prefab is assigned, it fails to
         * load.
         */
        FUNCTION(Script)
        bool Spawn(CWorld* World, const FTransform& Transform = FTransform{}) const;

        PROPERTY(Editable, Category = "Prefab")
        TSoftObjectPtr<CPrefab> PrefabInstance;
        
        PROPERTY(Editable, Category = "Prefab")
        bool bSpawnOnStartup = false;
        
        /** Seconds between automatic spawns, drawn fresh from [X, Y] after each one. X <= 0 disables them. */
        PROPERTY(Editable, Category = "Prefab")
        FVector2 SpawnTimeRange = FVector2{1.0f, 10.0f};

        /**
         * Countdown to the next automatic spawn, driven by SPrefabSpawnerSystem::Update. Runtime state:
         * not a PROPERTY, so it is neither serialized nor editable. Zero means "not armed yet" - the
         * system arms it the first time it sees the component rather than spawning immediately, so a
         * spawner added mid-game still waits out a full interval.
         */
        float TimeUntilNextSpawn = 0.0f;
    };
}
