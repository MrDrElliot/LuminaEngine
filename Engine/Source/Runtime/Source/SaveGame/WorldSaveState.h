#pragma once

#include "Core/Serialization/Archiver.h"
#include "World/ECS/Registry.h"

namespace Lumina::SaveGame
{
    // Remembers which entities came from the level, the ones a save can find again by id and record as destroyed.
    RUNTIME_API void RecordLevelEntities(ECS::FRegistry& Registry);

    RUNTIME_API bool IsLevelEntity(ECS::FRegistry& Registry, ECS::FEntity Entity);

    // Writes the registry's save game state, or reads one into the live registry, by the archive's direction.
    RUNTIME_API bool SerializeRegistry(FArchive& Ar, ECS::FRegistry& Registry);

    // The key a runtime-built entity is found by when it has no SaveKey, its names from the root down.
    RUNTIME_API FString GetEntityNamePath(ECS::FRegistry& Registry, ECS::FEntity Entity);
}
