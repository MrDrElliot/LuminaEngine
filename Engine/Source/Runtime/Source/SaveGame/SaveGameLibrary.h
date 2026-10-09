#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "SaveGameLibrary.generated.h"

namespace Lumina
{
    class CSaveGame;
    class CWorld;

    // Slots live in the project's Saved/SaveGames while developing and in the user's app data once shipped.
    REFLECT()
    class RUNTIME_API CSaveGameLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Writes through a temporary file, so a crash mid-save leaves the previous save intact.
        FUNCTION()
        static bool SaveGameToSlot(CSaveGame* SaveGame, const FString& SlotName, int32 UserIndex);

        // A new object of the class it was saved as, or null when the slot is empty, unreadable or that class is gone.
        FUNCTION()
        static CSaveGame* LoadGameFromSlot(const FString& SlotName, int32 UserIndex);

        FUNCTION()
        static bool DoesSaveGameExist(const FString& SlotName, int32 UserIndex);

        FUNCTION()
        static bool DeleteGameInSlot(const FString& SlotName, int32 UserIndex);

        FUNCTION()
        static FString GetSaveGameDirectory(int32 UserIndex);

        FUNCTION()
        static FString GetSlotFilePath(const FString& SlotName, int32 UserIndex);

        // Captures World into SaveGame and writes it at the end of the frame, safe from any system or thread.
        FUNCTION()
        static void RequestSaveGame(CWorld* World, CSaveGame* SaveGame, const FString& SlotName, int32 UserIndex);

        // Loads the slot and restores World at the end of the frame. SSaveGameCompletedEvent and OnWorldRestored report back.
        FUNCTION()
        static void RequestLoadGame(CWorld* World, const FString& SlotName, int32 UserIndex);

        // Slot names in the user's save directory, newest first.
        static void GetSlotNames(int32 UserIndex, TVector<FString>& OutSlots);

        static bool SaveGameToBytes(CSaveGame* SaveGame, TVector<uint8>& OutBytes);
        static CSaveGame* LoadGameFromBytes(const TVector<uint8>& Bytes);
    };
}
