#include "RuntimePCH.h"
#include "World/Subsystems/SaveGameSubsystem.h"

#include <algorithm>
#include <ctime>
#include "SaveGame/SaveGame.h"
#include "SaveGame/SaveGameArchive.h"
#include "SaveGame/SaveGameEvents.h"
#include "World/World.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Scripting/ScriptableObject.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 SlotMagic         = 0x5641534C;
        constexpr uint32 SlotFormatVersion = 1;
        constexpr FStringView SlotExtension = ".sav";

        bool IsSlotCharacter(char Character)
        {
            return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z')
                || (Character >= '0' && Character <= '9') || Character == ' ' || Character == '-' || Character == '_' || Character == '.';
        }

        // Anything a filename cannot hold becomes an underscore, so a slot name never leaves its directory.
        FString SanitizeSlotName(const FString& SlotName)
        {
            FString Clean;
            for (char Character : SlotName)
            {
                Clean += IsSlotCharacter(Character) ? Character : '_';
            }
            while (!Clean.IsEmpty() && (Clean[0] == '.' || Clean[0] == ' '))
            {
                Clean.RemoveAt(0);
            }
            while (!Clean.IsEmpty() && (Clean.Last() == '.' || Clean.Last() == ' '))
            {
                Clean.LeftChopInline(1);
            }
            return Clean;
        }

        FStringView View(const FString& String)
        {
            return FStringView(*String, String.Len());
        }

        void SendRequest(CWorld* World, const SSaveGameRequestEvent& Request)
        {
            if (World == nullptr)
            {
                LOG_WARN("A save game request for slot '{}' was given no world.", *Request.SlotName);
                return;
            }
            ECS::SendEvent(World->GetEvents(), Request);
        }
    }

    void CSaveGameSubsystem::RequestSaveGame(CSaveGame* SaveGame, const FString& SlotName, int32 UserIndex)
    {
        SSaveGameRequestEvent Request;
        Request.Action    = ESaveGameAction::Save;
        Request.SlotName  = SlotName;
        Request.UserIndex = UserIndex;
        Request.SaveGame  = SaveGame;
        SendRequest(GetWorld(), Move(Request));
    }

    void CSaveGameSubsystem::RequestLoadGame(const FString& SlotName, int32 UserIndex)
    {
        SSaveGameRequestEvent Request;
        Request.Action    = ESaveGameAction::Load;
        Request.SlotName  = SlotName;
        Request.UserIndex = UserIndex;
        SendRequest(GetWorld(), Move(Request));
    }
}
