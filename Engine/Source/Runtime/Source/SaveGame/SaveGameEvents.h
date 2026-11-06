#pragma once

#include "SaveGame.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "SaveGameEvents.generated.h"

namespace Lumina
{
    REFLECT()
    enum class ESaveGameAction : uint8
    {
        Save,
        Load,
    };

    // Queued for the world's CSaveGameSystem, which saves or loads at its next update, so any system on any thread may ask.
    REFLECT(Event)
    struct RUNTIME_API SSaveGameRequestEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ESaveGameAction Action = ESaveGameAction::Save;

        PROPERTY(Hidden)
        FString SlotName;

        PROPERTY(Hidden)
        int32 UserIndex = 0;

        // What a save captures the world into and writes. A load ignores it.
        PROPERTY(Hidden)
        TStrongObjectPtr<CSaveGame> SaveGame;
    };

    // Queued once a request has run. SaveGame is what was written, or what was read and restored into the world.
    REFLECT(Event)
    struct RUNTIME_API SSaveGameCompletedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ESaveGameAction Action = ESaveGameAction::Save;

        PROPERTY(Hidden)
        FString SlotName;

        PROPERTY(Hidden)
        int32 UserIndex = 0;

        PROPERTY(Hidden)
        TStrongObjectPtr<CSaveGame> SaveGame;

        PROPERTY(Hidden)
        bool bSucceeded = false;
    };
}
