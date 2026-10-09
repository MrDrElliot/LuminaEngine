#include "RuntimePCH.h"
#include "SaveGameLibrary.h"

#include <algorithm>
#include <ctime>
#include "SaveGame.h"
#include "SaveGameArchive.h"
#include "SaveGameEvents.h"
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
            while (!Clean.empty() && (Clean.front() == '.' || Clean.front() == ' '))
            {
                Clean.erase(Clean.begin());
            }
            while (!Clean.empty() && (Clean.back() == '.' || Clean.back() == ' '))
            {
                Clean.pop_back();
            }
            return Clean;
        }

        FStringView View(const FString& String)
        {
            return FStringView(String.c_str(), String.size());
        }

        // A parallel system's request waits for the sync point, like any event it raises.
        void SendRequest(CWorld* World, SSaveGameRequestEvent Request)
        {
            if (World == nullptr)
            {
                LOG_WARN("A save game request for slot '{}' was given no world.", Request.SlotName.c_str());
                return;
            }
            ECS::FEventDispatcher* const* Found = ECS::GetWorldRegistry(*World).Ctx().Find<ECS::FEventDispatcher*>();
            if (Found == nullptr || *Found == nullptr)
            {
                LOG_WARN("A save game request for slot '{}' went to a world that is not running.", Request.SlotName.c_str());
                return;
            }
            ECS::FEventDispatcher* Dispatcher = *Found;
            if (ECS::FCommandBus::ShouldDefer())
            {
                World->GetCommandBus().Enqueue([Dispatcher, Request = Move(Request)]() mutable
                {
                    Dispatcher->Trigger<SSaveGameRequestEvent>(Request);
                });
                return;
            }
            Dispatcher->Trigger<SSaveGameRequestEvent>(Request);
        }
    }

    void CSaveGameLibrary::RequestSaveGame(CWorld* World, CSaveGame* SaveGame, const FString& SlotName, int32 UserIndex)
    {
        SSaveGameRequestEvent Request;
        Request.Action    = ESaveGameAction::Save;
        Request.SlotName  = SlotName;
        Request.UserIndex = UserIndex;
        Request.SaveGame  = SaveGame;
        SendRequest(World, Move(Request));
    }

    void CSaveGameLibrary::RequestLoadGame(CWorld* World, const FString& SlotName, int32 UserIndex)
    {
        SSaveGameRequestEvent Request;
        Request.Action    = ESaveGameAction::Load;
        Request.SlotName  = SlotName;
        Request.UserIndex = UserIndex;
        SendRequest(World, Move(Request));
    }

    FString CSaveGameLibrary::GetSaveGameDirectory(int32 UserIndex)
    {
        FString Directory;
        if (GEngine != nullptr && !GEngine->GetProjectPath().empty())
        {
            const FFixedString Path = Paths::Combine(GEngine->GetProjectPath(), "Saved", "SaveGames");
            Directory.assign(Path.c_str(), Path.size());
        }
        else
        {
            const FStringView ProjectName = GEngine != nullptr ? GEngine->GetProjectName() : FStringView();
            Directory = Paths::GetUserDataDirectory();
            Directory += "/";
            Directory += ProjectName.empty() ? FString("Lumina") : FString(ProjectName.data(), ProjectName.size());
            Directory += "/SaveGames";
        }
        if (UserIndex > 0)
        {
            Directory += Format("/User{}", UserIndex);
        }
        return Directory;
    }

    FString CSaveGameLibrary::GetSlotFilePath(const FString& SlotName, int32 UserIndex)
    {
        const FString Clean = SanitizeSlotName(SlotName);
        if (Clean.empty())
        {
            LOG_WARN("Save game slot name '{}' has nothing a file name can use.", SlotName.c_str());
            return FString();
        }
        return GetSaveGameDirectory(UserIndex) + "/" + Clean + FString(SlotExtension.data(), SlotExtension.size());
    }

    bool CSaveGameLibrary::SaveGameToBytes(CSaveGame* SaveGame, TVector<uint8>& OutBytes)
    {
        OutBytes.clear();
        if (SaveGame == nullptr || SaveGame->GetClass() == nullptr)
        {
            LOG_WARN("Asked to save a null save game.");
            return false;
        }

        SaveGame->OnBeforeSave();
        SaveGame->SavedUnixTime = (int64)std::time(nullptr);

        FMemoryWriter Writer(OutBytes);
        uint32 Magic       = SlotMagic;
        uint32 Version     = SlotFormatVersion;
        int32  FileVersion = GPackageFileLuminaVersion.FileVersion;
        FName  ClassName   = SaveGame->GetClass()->GetName();
        Writer << Magic << Version << FileVersion << ClassName;

        FSaveGameArchive Ar(Writer);
        SaveGame::SerializeObjectState(Ar, SaveGame);
        return !Writer.HasError();
    }

    CSaveGame* CSaveGameLibrary::LoadGameFromBytes(const TVector<uint8>& Bytes)
    {
        FMemoryReader Reader(Bytes);
        uint32 Magic       = 0;
        uint32 Version     = 0;
        int32  FileVersion = 0;
        Reader << Magic << Version;
        if (Reader.HasError() || Magic != SlotMagic || Version == 0 || Version > SlotFormatVersion)
        {
            LOG_ERROR("Not a save game this engine can read (magic {:x}, version {}).", Magic, Version);
            return nullptr;
        }

        Reader << FileVersion;
        if (FileVersion > GPackageFileLuminaVersion.FileVersion)
        {
            LOG_ERROR("The save game was written by a newer engine (format {}, this engine reads up to {}).",
                FileVersion, GPackageFileLuminaVersion.FileVersion);
            return nullptr;
        }
        Reader.SetFileVersion(FileVersion);

        FName ClassName;
        Reader << ClassName;
        CClass* Class = FScriptableRegistry::ResolveClass(ClassName);
        if (Class == nullptr || !Class->IsChildOf(CSaveGame::StaticClass()))
        {
            LOG_ERROR("The save game's class '{}' is not a loaded CSaveGame class.", ClassName.c_str());
            return nullptr;
        }

        CSaveGame* SaveGame = NewObject<CSaveGame>(Class, nullptr, NAME_None, FGuid::New(), OF_Transient);
        FSaveGameArchive Ar(Reader);
        SaveGame::SerializeObjectState(Ar, SaveGame);
        if (Reader.HasError())
        {
            LOG_ERROR("The save game is damaged and could not be read to the end.");
            return nullptr;
        }

        SaveGame->OnAfterLoad();
        return SaveGame;
    }

    bool CSaveGameLibrary::SaveGameToSlot(CSaveGame* SaveGame, const FString& SlotName, int32 UserIndex)
    {
        const FString Path = GetSlotFilePath(SlotName, UserIndex);
        TVector<uint8> Bytes;
        if (Path.empty() || !SaveGameToBytes(SaveGame, Bytes))
        {
            return false;
        }

        Filesystem::MakeParentDirectoryTree(View(Path));
        if (!Filesystem::AtomicWriteFile(View(Path), Bytes))
        {
            LOG_ERROR("Could not write the save game to {}.", Path.c_str());
            return false;
        }
        LOG_INFO("Saved game slot '{}' to {} ({} bytes).", SlotName.c_str(), Path.c_str(), Bytes.size());
        return true;
    }

    CSaveGame* CSaveGameLibrary::LoadGameFromSlot(const FString& SlotName, int32 UserIndex)
    {
        const FString Path = GetSlotFilePath(SlotName, UserIndex);
        if (Path.empty() || !Filesystem::IsFile(View(Path)))
        {
            return nullptr;
        }

        TVector<uint8> Bytes;
        if (!Filesystem::ReadFile(Bytes, View(Path)))
        {
            LOG_ERROR("Could not read the save game at {}.", Path.c_str());
            return nullptr;
        }
        return LoadGameFromBytes(Bytes);
    }

    bool CSaveGameLibrary::DoesSaveGameExist(const FString& SlotName, int32 UserIndex)
    {
        const FString Path = GetSlotFilePath(SlotName, UserIndex);
        return !Path.empty() && Filesystem::IsFile(View(Path));
    }

    bool CSaveGameLibrary::DeleteGameInSlot(const FString& SlotName, int32 UserIndex)
    {
        const FString Path = GetSlotFilePath(SlotName, UserIndex);
        return !Path.empty() && Filesystem::IsFile(View(Path)) && Filesystem::RemoveFile(View(Path));
    }

    void CSaveGameLibrary::GetSlotNames(int32 UserIndex, TVector<FString>& OutSlots)
    {
        OutSlots.clear();
        const FString Directory = GetSaveGameDirectory(UserIndex);
        if (!Filesystem::IsDirectory(View(Directory)))
        {
            return;
        }

        TVector<TPair<int64, FString>> Found;
        Filesystem::IterateDirectory(View(Directory), [&](const Filesystem::FDirectoryEntry& Entry)
        {
            const FStringView Name = Entry.Name;
            if (!Entry.IsDirectory() && Name.size() > SlotExtension.size() && Name.ends_with(SlotExtension))
            {
                const FStringView Slot = Name.substr(0, Name.size() - SlotExtension.size());
                Found.push_back({ Entry.LastModifyTime, FString(Slot.data(), Slot.size()) });
            }
        });

        std::sort(Found.begin(), Found.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
        for (auto& Pair : Found)
        {
            OutSlots.push_back(Move(Pair.second));
        }
    }
}
