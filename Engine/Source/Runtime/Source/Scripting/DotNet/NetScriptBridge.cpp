#include "RuntimePCH.h"
#include "NetScriptBridge.h"

#include "Core/Object/ScriptClass.h"
#include "Networking/NetworkEvents.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "World/ECS/Registry.h"
#include "World/World.h"

namespace Lumina
{
    void TEventFanOut<SNetSessionEvent>::Deliver(ECS::FRegistry& Registry, const SNetSessionEvent& Event)
    {
        // Gathered first, since a listener may spawn the player's entity and grow the pool being walked.
        TVector<TStrongObjectPtr<CEntityScript>> Scripts;
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
            switch (Event.Kind)
            {
                case ENetSessionEventKind::ClientJoined: Script->OnConnected(Event.ConnectionId);    break;
                case ENetSessionEventKind::ClientLeft:   Script->OnDisconnected(Event.ConnectionId); break;
                case ENetSessionEventKind::JoinedHost:   Script->OnJoinedHost();                     break;
                case ENetSessionEventKind::LeftHost:     Script->OnLeftHost(Event.Reason);           break;
            }
        }
    }
}

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
