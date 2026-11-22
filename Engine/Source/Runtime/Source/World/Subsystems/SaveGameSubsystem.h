#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "SaveGameSubsystem.generated.h"

namespace Lumina
{
    class CSaveGame;

    // Save and load requests, served by the save game system.
    REFLECT()
    class RUNTIME_API CSaveGameSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        // Captures World into SaveGame and writes it at the end of the frame, safe from any system or thread.
        FUNCTION()
        void RequestSaveGame(CSaveGame* SaveGame, const FString& SlotName, int32 UserIndex);

        // Loads the slot and restores World at the end of the frame. SSaveGameCompletedEvent and OnWorldRestored report back.
        FUNCTION()
        void RequestLoadGame(const FString& SlotName, int32 UserIndex);

        // Before a save reads the world, so derived state can be folded into SaveGame fields.
        PROPERTY(Hidden)
        FScriptDelegate OnSaving;

        // After a load wrote the saved fields back into the world, so anything built from them is stale. Respawned scripts attach later and miss it.
        PROPERTY(Hidden)
        FScriptDelegate OnRestored;
    };
}
