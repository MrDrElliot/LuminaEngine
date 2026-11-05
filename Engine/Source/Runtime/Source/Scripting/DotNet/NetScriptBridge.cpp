#include "RuntimePCH.h"
#include "NetScriptBridge.h"

#include "Core/Object/ScriptClass.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "World/ECS/Registry.h"
#include "World/World.h"

namespace Lumina::NetScripts
{
    namespace
    {
        DotNet::TManagedExport<void (*)(void*, const char*, int32, int32)>          GDispatchSync("NetDispatchSync");

        // Null for a C++ script, which has no managed side to notify.
        void* ManagedHandleOf(CEntityScript* Script)
        {
            if (Script == nullptr || ToScriptClass(Script->GetClass()) == nullptr)
            {
                return nullptr;
            }
            return Scriptable::GetOrCreateInstance(Script);
        }
    }

    // Either language, since a C# [Rpc] method is a reflected function read through the same properties.
    void DispatchRpc(CEntityScript* Script, uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize)
    {
        if (Script != nullptr)
        {
            Script->ReceiveRpc(RpcId, CallerId, Payload, PayloadSize);
        }
    }

    void DispatchConnection(CWorld* World, uint32 ConnectionId, bool bJoined)
    {
        DispatchSession(World, bJoined ? ENetSessionEvent::ClientJoined : ENetSessionEvent::ClientLeft, ConnectionId, ENetLeaveReason::None);
    }

    void DispatchSession(CWorld* World, ENetSessionEvent Event, uint32 ConnectionId, ENetLeaveReason Reason)
    {
        if (World == nullptr)
        {
            return;
        }
        // Gathered first, since a listener may spawn the player's entity and grow the pool being walked.
        TVector<TStrongObjectPtr<CEntityScript>> Scripts;
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        for (ECS::FEntity Entity : Registry.View<SEntityScriptComponent>())
        {
            for (const TStrongObjectPtr<CEntityScript>& Script : Registry.Get<SEntityScriptComponent>(Entity).Scripts)
            {
                if (Script.Get() != nullptr && Script->IsReady())
                {
                    Scripts.Add(Script);
                }
            }
        }

        for (const TStrongObjectPtr<CEntityScript>& Script : Scripts)
        {
            switch (Event)
            {
                case ENetSessionEvent::ClientJoined: Script->OnConnected(ConnectionId);    break;
                case ENetSessionEvent::ClientLeft:   Script->OnDisconnected(ConnectionId); break;
                case ENetSessionEvent::JoinedHost:   Script->OnJoinedHost();               break;
                case ENetSessionEvent::LeftHost:     Script->OnLeftHost(Reason);           break;
            }
        }
    }

    void DispatchSyncChanging(CEntityScript* Script, const char* Names, uint32 NamesSize)
    {
        void* Handle = ManagedHandleOf(Script);
        if (Handle == nullptr)
        {
            if (Script != nullptr)
            {
                Script->SyncArriving();
            }
            return;
        }
        if (auto* Fn = GDispatchSync.Get())
        {
            Fn(Handle, Names, (int32)NamesSize, 0);
        }
    }

    void DispatchSyncChanged(CEntityScript* Script, const char* Names, uint32 NamesSize)
    {
        void* Handle = ManagedHandleOf(Script);
        if (Handle == nullptr)
        {
            if (Script != nullptr)
            {
                Script->SyncArrived(Names, NamesSize);
            }
            return;
        }
        if (auto* Fn = GDispatchSync.Get())
        {
            Fn(Handle, Names, (int32)NamesSize, 1);
        }
    }
}
