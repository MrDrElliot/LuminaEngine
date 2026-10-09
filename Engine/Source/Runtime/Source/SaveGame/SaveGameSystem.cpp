#include "RuntimePCH.h"
#include "SaveGameSystem.h"

#include "SaveGameLibrary.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        ECS::FEventDispatcher* WorldDispatcher(CWorld* World)
        {
            if (World == nullptr)
            {
                return nullptr;
            }
            ECS::FEventDispatcher* const* Found = ECS::GetWorldRegistry(*World).Ctx().Find<ECS::FEventDispatcher*>();
            return Found != nullptr ? *Found : nullptr;
        }
    }

    void SSaveGameSystem::Configure()
    {
        // Paused too, since a pause menu is where a player usually saves and loads.
        RequireUpdate(EUpdateStage::FrameEnd, EUpdatePriority::Low);
        RequireUpdate(EUpdateStage::Paused, EUpdatePriority::Low);
    }

    void SSaveGameSystem::OnStartup()
    {
        if (ECS::FEventDispatcher* Dispatcher = WorldDispatcher(GetWorld()))
        {
            Dispatcher->Sink<SSaveGameRequestEvent>().Connect<&SSaveGameSystem::OnRequest>(this);
            bConnected = true;
        }
    }

    void SSaveGameSystem::OnTeardown()
    {
        if (ECS::FEventDispatcher* Dispatcher = bConnected ? WorldDispatcher(GetWorld()) : nullptr)
        {
            Dispatcher->Sink<SSaveGameRequestEvent>().Disconnect<&SSaveGameSystem::OnRequest>(this);
        }
        bConnected = false;
        FScopeLock Lock(PendingLock);
        Pending.clear();
    }

    bool SSaveGameSystem::HasWork(EUpdateStage Stage)
    {
        FScopeLock Lock(PendingLock);
        return !Pending.empty();
    }

    void SSaveGameSystem::OnRequest(const SSaveGameRequestEvent& Request)
    {
        FScopeLock Lock(PendingLock);
        Pending.push_back(Request);
    }

    void SSaveGameSystem::OnUpdate()
    {
        TVector<SSaveGameRequestEvent> Requests;
        {
            FScopeLock Lock(PendingLock);
            Requests.swap(Pending);
        }
        for (const SSaveGameRequestEvent& Request : Requests)
        {
            Run(Request);
        }
    }

    void SSaveGameSystem::Run(const SSaveGameRequestEvent& Request)
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

        if (ECS::FEventDispatcher* Dispatcher = WorldDispatcher(World))
        {
            Dispatcher->Trigger<SSaveGameCompletedEvent>(Done);
        }
    }
}
