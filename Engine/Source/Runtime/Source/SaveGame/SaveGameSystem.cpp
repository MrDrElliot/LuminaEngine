#include "RuntimePCH.h"
#include "SaveGameSystem.h"

#include "SaveGameLibrary.h"
#include "World/World.h"

namespace Lumina
{
    void SSaveGameSystem::Configure()
    {
        // Paused too, since a pause menu is where a player usually saves and loads.
        RequireUpdate(EUpdateStage::FrameEnd);
        RequireUpdate(EUpdateStage::Paused);
        Requests = ReadsEvent<SSaveGameRequestEvent>();
        SendsEvent<SSaveGameCompletedEvent>();
    }

    bool SSaveGameSystem::HasWork(const SSystemContext& Context)
    {
        return Context.HasUnreadEvents(Requests);
    }

    void SSaveGameSystem::OnUpdate(const SSystemContext& Context)
    {
        // A restore spawns and destroys, but nothing commits until this update returns, so the range holds still.
        for (const SSaveGameRequestEvent& Request : Context.ReadEvents(Requests))
        {
            Run(Context, Request);
        }
    }

    void SSaveGameSystem::Run(const SSystemContext& Context, const SSaveGameRequestEvent& Request)
    {
        CWorld* World = GetWorld();

        SSaveGameCompletedEvent Done;
        Done.Action    = Request.Action;
        Done.SlotName  = Request.SlotName;
        Done.UserIndex = Request.UserIndex;

        if (Request.Action == ESaveGameAction::Save)
        {
            Done.SaveGame = Request.SaveGame;
            if (Request.SaveGame.Get() == nullptr)
            {
                LOG_WARN("A save request for slot '{}' carried no save game.", Request.SlotName.c_str());
            }
            else
            {
                Done.bSucceeded = Request.SaveGame->CaptureWorld(World)
                    && CSaveGameLibrary::SaveGameToSlot(Request.SaveGame.Get(), Request.SlotName, Request.UserIndex);
            }
        }
        else if (CSaveGame* Loaded = CSaveGameLibrary::LoadGameFromSlot(Request.SlotName, Request.UserIndex))
        {
            Done.SaveGame = Loaded;
            Done.bSucceeded = !Loaded->HasWorldState(World) || Loaded->RestoreWorld(World);
        }

        Context.SendEvent(Done);
    }
}
