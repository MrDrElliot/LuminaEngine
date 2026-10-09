#pragma once

#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "SaveGameComponent.generated.h"

namespace Lumina
{
    // Puts the whole entity in save games, its place too, and recreates it on load when the game spawned it at runtime.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SSaveGameComponent
    {
        GENERATED_BODY()

        // Finds the entity again on load by this name, for one the game builds itself each run instead of respawning.
        PROPERTY(Editable, Category = "Save Game")
        FName SaveKey;

        PROPERTY(Editable, Category = "Save Game")
        bool bSaveTransform = true;

        // A runtime spawn is destroyed and recreated from the save, children included, unless it has a SaveKey.
        PROPERTY(Editable, Category = "Save Game")
        bool bRespawn = true;
    };
}
