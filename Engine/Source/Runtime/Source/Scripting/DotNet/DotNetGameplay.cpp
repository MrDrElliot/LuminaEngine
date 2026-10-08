#include "Platform/GenericPlatform.h"
#include "World/ECS/Registry.h"
#include "Scripting/DotNet/LayoutRegistry.h"
#include "Containers/String.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/CameraSystem.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "AI/Navigation/NavTypes.h"
#include "GameplayTags/GameplayTagRegistry.h"
#include "GameplayTags/GameplayTagComponent.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineURL.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/EntityScript.h"
#include "Core/Reflection/Type/Function.h"
#include "Scripting/ScriptableObject.h"
#include "Core/Object/ScriptClass.h"
#include "Scripting/DotNet/NetScriptBridge.h"
#include "Networking/INetworkRuntime.h"
#include "Input/InputActionMap.h"
#include "Input/InputQuery.h"
#include "Input/InputViewport.h"
#include "Input/InputContext.h"
#include "Input/InputMode.h"
#include "Events/MouseCodes.h"

// World is an opaque pointer and Entity an entity id, matching the component ops convention.

using namespace Lumina;
using namespace Lumina::DotNet;   // AsWorld / AsEntity / ToId

// Only the not-yet-reflectable surface lives here, the rest is generated from CWorld's declarations.

// The world's own FSystemContext, so a managed system can be handed a valid context outside a tick.
LUMINA_DOTNET_EXPORT(const void*, World_GetSystemContext)(uint64 World)
{
    CWorld* W = AsWorld(World);
    return W ? &W->GetSystemContext() : nullptr;
}

// Game, the engine-level session operations.

// Valid only for the duration of the OnUpdate crossing, forwarding to the matching context method.

// The managed boundary caught a throw from OnAttach or OnReady, and only it knows which script raised it.
LUMINA_DOTNET_EXPORT(void, EntityScript_MarkFaulted)(CEntityScript* Script)
{
    if (Script != nullptr)
    {
        Script->MarkFaulted();
    }
}

LUMINA_DOTNET_EXPORT(float, SystemContext_GetDeltaTime)(const FSystemContext* Ctx)
{
    return Ctx ? (float)Ctx->GetDeltaTime() : 0.0f;
}

LUMINA_DOTNET_EXPORT(double, SystemContext_GetTime)(const FSystemContext* Ctx)
{
    return Ctx ? Ctx->GetTime() : 0.0;
}

LUMINA_DOTNET_EXPORT(uint32, SystemContext_Create)(const FSystemContext* Ctx)
{
    return Ctx ? ToId(Ctx->Create()) : ToId(ECS::NullEntity);
}

LUMINA_DOTNET_EXPORT(void, SystemContext_Destroy)(const FSystemContext* Ctx, uint32 Entity)
{
    if (Ctx)
    {
        Ctx->Destroy(AsEntity(Entity));
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_SetEntityLocation)(const FSystemContext* Ctx, uint32 Entity, FVector3 Location)
{
    // The scheduler owns a non-const context, so removing const here is safe.
    if (Ctx)
    {
        const_cast<FSystemContext*>(Ctx)->SetEntityLocation(AsEntity(Entity), Location);
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_DrawDebugLine)(const FSystemContext* Ctx, FVector3 Start, FVector3 End, FVector4 Color)
{
    if (Ctx)
    {
        Ctx->DrawDebugLine(Start, End, Color);
    }
}

// The process-global registry is the single source of truth, and id 0 means none.

// Queries are hierarchical, so an entity tagged with a leaf matches a query on its parent.

// IsEnabled lets the managed side skip per-script scope calls when nobody is recording.

// A binding resolves its name once per settings generation, so it costs no crossing per frame.

// Networking. Each call answers as a standalone world when no netcode is installed.

LUMINA_DOTNET_EXPORT(uint32, Net_GetLocalConnection)(uint64 World)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return (Net && AsWorld(World)) ? Net->GetLocalConnectionId(AsWorld(World)) : 0u;
}

// Returns the total, so a caller whose buffer was too small can retry with the right size.
LUMINA_DOTNET_EXPORT(int32, Net_GetConnections)(uint64 World, uint32* Out, int32 Capacity)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    if (Net == nullptr || AsWorld(World) == nullptr)
    {
        return 0;
    }
    TVector<uint32> Ids;
    Net->GetConnectionIds(AsWorld(World), Ids);
    for (int32 i = 0; i < Capacity && i < (int32)Ids.size(); ++i)
    {
        Out[i] = Ids[i];
    }
    return (int32)Ids.size();
}

LUMINA_DOTNET_EXPORT(int32, Net_IsEntityNetworked)(uint64 World, uint32 Entity)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return (Net && AsWorld(World) && Net->IsEntityNetworked(AsWorld(World), AsEntity(Entity))) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint32, Net_GetOwner)(uint64 World, uint32 Entity)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return (Net && AsWorld(World)) ? Net->GetEntityOwner(AsWorld(World), AsEntity(Entity)) : 0u;
}

LUMINA_DOTNET_EXPORT(int32, Net_SetOwner)(uint64 World, uint32 Entity, uint32 ConnectionId)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return (Net && AsWorld(World) && Net->SetEntityOwner(AsWorld(World), AsEntity(Entity), ConnectionId)) ? 1 : 0;
}

// Called from a [Sync] setter, so it takes the script and finds the entity itself.
LUMINA_DOTNET_EXPORT(void, Net_MarkScriptDirty)(CEntityScript* Script)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    if (Net != nullptr && Script != nullptr && Script->GetWorld() != nullptr && Script->IsAttached())
    {
        Net->MarkEntityDirty(Script->GetWorld(), Script->GetOwningEntity());
    }
}

LUMINA_DOTNET_EXPORT(void, Net_MarkEntityDirty)(uint64 World, uint32 Entity)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    if (Net != nullptr && AsWorld(World) != nullptr)
    {
        Net->MarkEntityDirty(AsWorld(World), AsEntity(Entity));
    }
}

LUMINA_DOTNET_EXPORT(uint32, Net_EntityToNetId)(uint64 World, uint32 Entity)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return (Net && AsWorld(World)) ? Net->EntityToNetId(AsWorld(World), AsEntity(Entity)) : 0u;
}

LUMINA_DOTNET_EXPORT(uint32, Net_NetIdToEntity)(uint64 World, uint32 NetId)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return ToId((Net && AsWorld(World)) ? Net->NetIdToEntity(AsWorld(World), NetId) : ECS::NullEntity);
}

LUMINA_DOTNET_EXPORT(int32, Net_IsJoined)(uint64 World)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return (Net && AsWorld(World) && Net->IsJoined(AsWorld(World))) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint32, Net_GetOwnedPawn)(uint64 World, uint32 ConnectionId)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    return ToId((Net && AsWorld(World)) ? Net->FindOwnedPawn(AsWorld(World), ConnectionId) : ECS::NullEntity);
}

// A C# [Rpc] method is a reflected function, found once per type and routed exactly as NET_RPC routes a C++ one.
LUMINA_DOTNET_EXPORT(const void*, Net_FindRpc)(CEntityScript* Script, const char* Name, int32 Len)
{
    if (Script == nullptr || Script->GetClass() == nullptr || Name == nullptr || Len <= 0)
    {
        return nullptr;
    }
    const FFunction* Function = Script->GetClass()->FindFunction(FName(FString(Name, (size_t)Len).c_str()));
    return (Function != nullptr && Function->IsRpc()) ? Function : nullptr;
}

// The ERpcRoute for this call, which also consumes the mark a received call leaves.
LUMINA_DOTNET_EXPORT(int32, Net_RpcPlan)(CEntityScript* Script, const void* Function)
{
    if (Script == nullptr || Function == nullptr)
    {
        return (int32)ERpcRoute::RunHere;
    }
    return (int32)Script->PlanRpc(*static_cast<const FFunction*>(Function));
}

// A default call frame the caller fills slot by slot and hands to Net_RpcSend, which frees it.
LUMINA_DOTNET_EXPORT(void*, Net_RpcFrame)(const void* Function)
{
    const FFunction* Typed = static_cast<const FFunction*>(Function);
    if (Typed == nullptr)
    {
        return nullptr;
    }
    void* Frame = Memory::Malloc(std::max<size_t>(Typed->GetParmsSize(), 1), alignof(std::max_align_t));
    Memory::Memzero(Frame, std::max<size_t>(Typed->GetParmsSize(), 1));
    Typed->InitializeFrame(Frame);
    return Frame;
}

LUMINA_DOTNET_EXPORT(void, Net_RpcSend)(CEntityScript* Script, const void* Function, void* Frame)
{
    const FFunction* Typed = static_cast<const FFunction*>(Function);
    if (Typed == nullptr || Frame == nullptr)
    {
        return;
    }
    if (Script != nullptr)
    {
        Script->SendRpcFrame(*Typed, Frame);
    }
    Typed->DestructFrame(Frame);
    Memory::Free(Frame);
}

// The FromOwner Sync field a C# setter forwards, found once per type.
LUMINA_DOTNET_EXPORT(void*, Net_FindSyncField)(CEntityScript* Script, const char* Name, int32 Len)
{
    if (Script == nullptr || Script->GetClass() == nullptr || Name == nullptr || Len <= 0)
    {
        return nullptr;
    }
    FProperty* Field = Script->GetClass()->GetProperty(FName(FString(Name, (size_t)Len).c_str()));
    return (Field != nullptr && CEntityScript::IsSyncFromOwner(Field)) ? Field : nullptr;
}

LUMINA_DOTNET_EXPORT(void, Net_SendSync)(CEntityScript* Script, void* Field)
{
    if (Script != nullptr && Field != nullptr)
    {
        Script->SendSync(static_cast<FProperty*>(Field), false);
    }
}

LUMINA_DOTNET_EXPORT(uint32, Net_RpcCaller)(CEntityScript* Script)
{
    return Script != nullptr ? Script->GetRpcCaller() : CEntityScript::GetReceivingCaller();
}

LUMINA_DOTNET_EXPORT(int32, Net_IsReceivingRpc)()
{
    return CEntityScript::IsReceivingRpc() ? 1 : 0;
}

namespace Lumina::NetScripts
{
    namespace
    {
        DotNet::TManagedExport<void (*)(void*, int32, uint32, int32)>               GDispatchSession("NetDispatchSession");
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
        auto* Fn = GDispatchSession.Get();

        // Gathered first, since a listener may spawn the player's entity and grow the pool being walked.
        TVector<TStrongObjectPtr<CEntityScript>> Scripts;
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (ECS::FEntity Entity : Registry.View<SEntityScriptComponent>())
        {
            for (const TStrongObjectPtr<CEntityScript>& Script : Registry.Get<SEntityScriptComponent>(Entity).Scripts)
            {
                if (Script.Get() != nullptr && Script->IsReady())
                {
                    Scripts.push_back(Script);
                }
            }
        }

        for (const TStrongObjectPtr<CEntityScript>& Script : Scripts)
        {
            if (void* Handle = ManagedHandleOf(Script.Get()))
            {
                if (Fn != nullptr)
                {
                    Fn(Handle, (int32)Event, ConnectionId, (int32)Reason);
                }
                continue;
            }

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

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(World_GetSystemContext),
    LUMINA_DOTNET_SIG(EntityScript_MarkFaulted),
    LUMINA_DOTNET_SIG(SystemContext_GetDeltaTime),
    LUMINA_DOTNET_SIG(SystemContext_GetTime),
    LUMINA_DOTNET_SIG(SystemContext_Create),
    LUMINA_DOTNET_SIG(SystemContext_Destroy),
    LUMINA_DOTNET_SIG(SystemContext_SetEntityLocation),
    LUMINA_DOTNET_SIG(SystemContext_DrawDebugLine),
    LUMINA_DOTNET_SIG(Net_GetLocalConnection),
    LUMINA_DOTNET_SIG(Net_GetConnections),
    LUMINA_DOTNET_SIG(Net_IsEntityNetworked),
    LUMINA_DOTNET_SIG(Net_GetOwner),
    LUMINA_DOTNET_SIG(Net_SetOwner),
    LUMINA_DOTNET_SIG(Net_MarkScriptDirty),
    LUMINA_DOTNET_SIG(Net_MarkEntityDirty),
    LUMINA_DOTNET_SIG(Net_EntityToNetId),
    LUMINA_DOTNET_SIG(Net_NetIdToEntity),
    LUMINA_DOTNET_SIG(Net_IsJoined),
    LUMINA_DOTNET_SIG(Net_GetOwnedPawn),
    LUMINA_DOTNET_SIG(Net_FindRpc),
    LUMINA_DOTNET_SIG(Net_RpcPlan),
    LUMINA_DOTNET_SIG(Net_RpcFrame),
    LUMINA_DOTNET_SIG(Net_RpcSend),
    LUMINA_DOTNET_SIG(Net_FindSyncField),
    LUMINA_DOTNET_SIG(Net_SendSync),
    LUMINA_DOTNET_SIG(Net_RpcCaller),
    LUMINA_DOTNET_SIG(Net_IsReceivingRpc)
);
