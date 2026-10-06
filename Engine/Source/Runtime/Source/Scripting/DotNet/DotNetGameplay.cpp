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
#include "World/Entity/Components/RelationshipComponent.h"
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

LUMINA_DOTNET_EXPORT(int32, Net_SendRpc)(CEntityScript* Script, uint32 RpcId, uint32 Target, uint32 Flags, const uint8* Payload, int32 PayloadSize)
{
    INetworkRuntime* Net = GetNetworkRuntime();
    if (Net == nullptr || Script == nullptr || PayloadSize < 0)
    {
        return 0;
    }
    return Net->SendScriptRpc(Script, RpcId, (ERpcTarget)Target, (uint8)Flags, Payload, (uint32)PayloadSize) ? 1 : 0;
}

namespace Lumina::NetScripts
{
    namespace
    {
        DotNet::TManagedExport<void (*)(void*, uint32, uint32, const uint8*, int32)> GDispatchRpc("NetDispatchRpc");
        DotNet::TManagedExport<void (*)(void*, uint32, int32)>                      GDispatchConnection("NetDispatchConnection");
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

    void DispatchRpc(CEntityScript* Script, uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize)
    {
        auto* Fn = GDispatchRpc.Get();
        void* Handle = ManagedHandleOf(Script);
        if (Fn != nullptr && Handle != nullptr)
        {
            Fn(Handle, RpcId, CallerId, Payload, (int32)PayloadSize);
        }
    }

    void DispatchConnection(CWorld* World, uint32 ConnectionId, bool bJoined)
    {
        auto* Fn = GDispatchConnection.Get();
        if (Fn == nullptr || World == nullptr)
        {
            return;
        }

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
                Fn(Handle, ConnectionId, bJoined ? 1 : 0);
            }
        }
    }

    void DispatchSyncChanging(CEntityScript* Script, const char* Names, uint32 NamesSize)
    {
        auto* Fn = GDispatchSync.Get();
        void* Handle = ManagedHandleOf(Script);
        if (Fn != nullptr && Handle != nullptr)
        {
            Fn(Handle, Names, (int32)NamesSize, 0);
        }
    }

    void DispatchSyncChanged(CEntityScript* Script, const char* Names, uint32 NamesSize)
    {
        auto* Fn = GDispatchSync.Get();
        void* Handle = ManagedHandleOf(Script);
        if (Fn != nullptr && Handle != nullptr)
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
    LUMINA_DOTNET_SIG(Net_SendRpc)
);
