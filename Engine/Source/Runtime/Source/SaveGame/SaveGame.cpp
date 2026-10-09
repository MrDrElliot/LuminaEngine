#include "RuntimePCH.h"
#include "SaveGame.h"

#include "SaveGameEvents.h"
#include "WorldSaveState.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        FString WorldKey(const CWorld* World)
        {
            return FString(World->GetName().c_str());
        }

        // Systems hear it through a virtual, which a C# system can override, and anything else through the event.
        template<typename TEvent>
        void Notify(CWorld* World, CSaveGame* SaveGame, void (CEntitySystem::*Hook)(CSaveGame*))
        {
            for (const TStrongObjectPtr<CEntitySystem>& System : World->Systems)
            {
                if (System.Get() != nullptr)
                {
                    (System.Get()->*Hook)(SaveGame);
                }
            }
            if (ECS::FEventDispatcher* const* Dispatcher = ECS::GetWorldRegistry(*World).Ctx().Find<ECS::FEventDispatcher*>())
            {
                TEvent Event;
                Event.SaveGame = SaveGame;
                (*Dispatcher)->Trigger<TEvent>(Event);
            }
        }
    }

    bool CSaveGame::CaptureWorld(CWorld* World)
    {
        if (World == nullptr)
        {
            LOG_WARN("CSaveGame::CaptureWorld was given no world.");
            return false;
        }
        Notify<SWorldSavingEvent>(World, this, &CEntitySystem::OnWorldSaving);
        return CaptureRegistry(WorldKey(World), ECS::GetWorldRegistry(*World));
    }

    bool CSaveGame::RestoreWorld(CWorld* World)
    {
        if (World == nullptr)
        {
            LOG_WARN("CSaveGame::RestoreWorld was given no world.");
            return false;
        }
        if (!RestoreRegistry(WorldKey(World), ECS::GetWorldRegistry(*World)))
        {
            return false;
        }
        Notify<SWorldRestoredEvent>(World, this, &CEntitySystem::OnWorldRestored);
        return true;
    }

    bool CSaveGame::HasWorldState(CWorld* World) const
    {
        return World != nullptr && WorldStates.find(WorldKey(World)) != WorldStates.end();
    }

    void CSaveGame::ClearWorldStates()
    {
        WorldStates.clear();
    }

    bool CSaveGame::CaptureRegistry(const FString& Key, ECS::FRegistry& Registry)
    {
        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        if (!SaveGame::SerializeRegistry(Writer, Registry))
        {
            return false;
        }
        WorldStates[Key] = Move(Bytes);
        return true;
    }

    bool CSaveGame::RestoreRegistry(const FString& Key, ECS::FRegistry& Registry) const
    {
        const auto Found = WorldStates.find(Key);
        if (Found == WorldStates.end())
        {
            return false;
        }
        FMemoryReader Reader(Found->second);
        Reader.SetFileVersion(LoadedFileVersion);
        return SaveGame::SerializeRegistry(Reader, Registry);
    }

    void CSaveGame::Serialize(FArchive& Ar)
    {
        CObject::Serialize(Ar);

        if (Ar.IsReading())
        {
            LoadedFileVersion = Ar.GetFileVersion();
        }

        uint32 Count = (uint32)WorldStates.size();
        Ar << Count;
        if (Ar.IsWriting())
        {
            for (auto& [Key, Bytes] : WorldStates)
            {
                FString Name = Key;
                int64 Size = (int64)Bytes.size();
                Ar << Name << Size;
                Ar.Serialize(Bytes.data(), Size);
            }
            return;
        }

        WorldStates.clear();
        if (!Ar.CanHoldCount(Count, sizeof(int64)))
        {
            Ar.SetHasError(true);
            return;
        }
        for (uint32 Index = 0; Index < Count && !Ar.HasError(); ++Index)
        {
            FString Name;
            int64 Size = 0;
            Ar << Name << Size;
            if (Size < 0 || !Ar.CanHoldCount((uint64)Size))
            {
                Ar.SetHasError(true);
                return;
            }
            TVector<uint8>& Bytes = WorldStates[Name];
            Bytes.resize((size_t)Size);
            Ar.Serialize(Bytes.data(), Size);
        }
    }
}
