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
            return FString(*World->GetName());
        }

        // Runs inline rather than queued, since a system folds its state in before the capture reads it.
        void Notify(CWorld* World, CSaveGame* SaveGame, void (CEntitySystem::*Hook)(CSaveGame*))
        {
            for (const TStrongObjectPtr<CEntitySystem>& System : World->Systems)
            {
                if (System.Get() != nullptr)
                {
                    (System.Get()->*Hook)(SaveGame);
                }
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
        Notify(World, this, &CEntitySystem::OnWorldSaving);
        return CaptureRegistry(WorldKey(World), World->GetEntityRegistry());
    }

    bool CSaveGame::RestoreWorld(CWorld* World)
    {
        if (World == nullptr)
        {
            LOG_WARN("CSaveGame::RestoreWorld was given no world.");
            return false;
        }
        if (!RestoreRegistry(WorldKey(World), World->GetEntityRegistry()))
        {
            return false;
        }
        Notify(World, this, &CEntitySystem::OnWorldRestored);
        return true;
    }

    bool CSaveGame::HasWorldState(CWorld* World) const
    {
        return World != nullptr && WorldStates.Contains(WorldKey(World));
    }

    void CSaveGame::ClearWorldStates()
    {
        WorldStates.Reset();
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
        const auto Found = WorldStates.Find(Key);
        if (Found == nullptr)
        {
            return false;
        }
        FMemoryReader Reader(*Found);
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

        uint32 Count = (uint32)WorldStates.Num();
        Ar << Count;
        if (Ar.IsWriting())
        {
            for (auto& [Key, Bytes] : WorldStates)
            {
                FString Name = Key;
                int64 Size = Bytes.Num();
                Ar << Name << Size;
                Ar.Serialize(Bytes.GetData(), Size);
            }
            return;
        }

        WorldStates.Reset();
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
            Bytes.SetNum(IntCastChecked<int32>(Size));
            Ar.Serialize(Bytes.GetData(), Size);
        }
    }
}
