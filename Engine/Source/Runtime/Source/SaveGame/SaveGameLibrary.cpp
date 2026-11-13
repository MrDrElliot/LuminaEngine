#include "RuntimePCH.h"
#include "SaveGameLibrary.h"

#include <ctime>
#include "SaveGame.h"
#include "SaveGameArchive.h"
#include "SaveGameEvents.h"
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
    }

    FString CSaveGameLibrary::GetSaveGameDirectory(int32 UserIndex)
    {
        FFixedString Directory;
        if (Global::GEngine != nullptr && !Global::GEngine->GetProjectPath().IsEmpty())
        {
            Directory = Paths::Combine(Global::GEngine->GetProjectPath(), "Saved", "SaveGames");
        }
        else
        {
            const FStringView ProjectName = Global::GEngine != nullptr ? Global::GEngine->GetProjectName() : FStringView();
            Directory = Paths::Combine(View(Paths::GetUserDataDirectory()), ProjectName.IsEmpty() ? FStringView("Lumina") : ProjectName, "SaveGames");
        }
        if (UserIndex > 0)
        {
            Directory = Paths::Combine(Directory, View(Format("User{}", UserIndex)));
        }
        return FString(*Directory, Directory.Len());
    }

    FString CSaveGameLibrary::GetSlotFilePath(const FString& SlotName, int32 UserIndex)
    {
        const FString Clean = SanitizeSlotName(SlotName);
        if (Clean.IsEmpty())
        {
            LOG_WARN("Save game slot name '{}' has nothing a file name can use.", *SlotName);
            return FString();
        }
        return GetSaveGameDirectory(UserIndex) + "/" + Clean + FString(SlotExtension.GetData(), SlotExtension.Len());
    }

    bool CSaveGameLibrary::SaveGameToBytes(CSaveGame* SaveGame, TVector<uint8>& OutBytes)
    {
        OutBytes.Reset();
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
        int32  FileVersion = Global::GPackageFileLuminaVersion.FileVersion;
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
        if (FileVersion > Global::GPackageFileLuminaVersion.FileVersion)
        {
            LOG_ERROR("The save game was written by a newer engine (format {}, this engine reads up to {}).",
                FileVersion, Global::GPackageFileLuminaVersion.FileVersion);
            return nullptr;
        }
        Reader.SetFileVersion(FileVersion);

        FName ClassName;
        Reader << ClassName;
        CClass* Class = FScriptableRegistry::ResolveClass(ClassName);
        if (Class == nullptr || !Class->IsChildOf(CSaveGame::StaticClass()))
        {
            LOG_ERROR("The save game's class '{}' is not a loaded CSaveGame class.", *ClassName);
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
        if (Path.IsEmpty() || !SaveGameToBytes(SaveGame, Bytes))
        {
            return false;
        }

        Filesystem::MakeParentDirectoryTree(View(Path));
        if (!Filesystem::AtomicWriteFile(View(Path), Bytes))
        {
            LOG_ERROR("Could not write the save game to {}.", *Path);
            return false;
        }
        LOG_INFO("Saved game slot '{}' to {} ({} bytes).", *SlotName, *Path, Bytes.Num());
        return true;
    }

    CSaveGame* CSaveGameLibrary::LoadGameFromSlot(const FString& SlotName, int32 UserIndex)
    {
        const FString Path = GetSlotFilePath(SlotName, UserIndex);
        if (Path.IsEmpty() || !Filesystem::IsFile(View(Path)))
        {
            return nullptr;
        }

        TVector<uint8> Bytes;
        if (!Filesystem::ReadFile(Bytes, View(Path)))
        {
            LOG_ERROR("Could not read the save game at {}.", *Path);
            return nullptr;
        }
        return LoadGameFromBytes(Bytes);
    }

    bool CSaveGameLibrary::DoesSaveGameExist(const FString& SlotName, int32 UserIndex)
    {
        const FString Path = GetSlotFilePath(SlotName, UserIndex);
        return !Path.IsEmpty() && Filesystem::IsFile(View(Path));
    }

    bool CSaveGameLibrary::DeleteGameInSlot(const FString& SlotName, int32 UserIndex)
    {
        const FString Path = GetSlotFilePath(SlotName, UserIndex);
        return !Path.IsEmpty() && Filesystem::IsFile(View(Path)) && Filesystem::RemoveFile(View(Path));
    }

    void CSaveGameLibrary::GetSlotNames(int32 UserIndex, TVector<FString>& OutSlots)
    {
        OutSlots.Reset();
        const FString Directory = GetSaveGameDirectory(UserIndex);
        if (!Filesystem::IsDirectory(View(Directory)))
        {
            return;
        }

        TVector<TPair<int64, FString>> Found;
        Filesystem::IterateDirectory(View(Directory), [&](const Filesystem::FDirectoryEntry& Entry)
        {
            const FStringView Name = Entry.Name;
            if (!Entry.IsDirectory() && Name.Len() > SlotExtension.Len() && Name.EndsWith(SlotExtension))
            {
                const FStringView Slot = Name.Mid(0, Name.Len() - SlotExtension.Len());
                Found.Add({ Entry.LastModifyTime, FString(Slot.GetData(), Slot.Len()) });
            }
        });

        Algo::Sort(Found, [](const auto& A, const auto& B) { return A.Key > B.Key; });
        for (auto& [ModifyTime, Slot] : Found)
        {
            OutSlots.Add(Move(Slot));
        }
    }
}
