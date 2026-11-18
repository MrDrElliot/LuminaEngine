#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "SimulationComponents.generated.h"

namespace Lumina
{
    // An entity the deterministic simulation owns. SimId is the same on every peer, where an entity handle is not.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SSimEntity
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly)
        uint32 SimId = 0;

        // The tick that spawned it, so a rewind to before it removes it again.
        PROPERTY(ReadOnly)
        uint32 SpawnTick = 0;
    };

    // Destroyed by the simulation at Tick but kept, without its rollback state, until no rewind can reach that tick.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SSimPendingDestroy
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly)
        uint32 Tick = 0;

        // Hidden by the destroy rather than by the game, so a rewind that revives it shows it again.
        PROPERTY(ReadOnly)
        bool bAddedDisabledTag = false;
    };
}
