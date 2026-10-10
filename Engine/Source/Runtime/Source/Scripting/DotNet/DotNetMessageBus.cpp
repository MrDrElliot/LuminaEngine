#include "RuntimePCH.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/Class.h"
#include "GameplayTags/GameplayMessageBus.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "World/World.h"

using namespace Lumina;
using Lumina::DotNet::AsWorld;

namespace
{
    void FreeManagedListener(void* Context)
    {
        if (GFreeManagedDelegateContext != nullptr)
        {
            GFreeManagedDelegateContext(Context);
        }
    }
}

LUMINA_DOTNET_EXPORT(uint64, MessageBus_TypeKey)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return 0;
    }
    return MakeGameplayMessageType(FName(FStringView(Name, (size_t)Len)));
}

// The managed side checks the world is alive first, and from here the bus owns Context and frees it.
LUMINA_DOTNET_EXPORT(uint64, MessageBus_Subscribe)(uint64 World, uint32 Channel, uint32 Owner, uint64 Type, uint32 Match, void* Thunk, void* Context)
{
    if (World == 0 || Thunk == nullptr)
    {
        FreeManagedListener(Context);
        return 0;
    }

    FGameplayMessageBus::FSubscribeParams Params;
    Params.Channel  = Channel;
    Params.Owner    = ECS::FEntity::FromPacked(Owner);
    Params.Type     = Type;
    Params.Match    = Match == (uint32)EGameplayTagMatch::Exact ? EGameplayTagMatch::Exact : EGameplayTagMatch::Partial;
    Params.Invoke   = reinterpret_cast<FGameplayMessageBus::FInvoke>(Thunk);
    Params.Context  = Context;
    Params.Destroy  = &FreeManagedListener;
    Params.bManaged = true;
    return AsWorld(World)->GetMessageBus().Subscribe(Params);
}

LUMINA_DOTNET_EXPORT(uint32, MessageBus_Unsubscribe)(uint64 World, uint64 Id)
{
    return World != 0 && AsWorld(World)->GetMessageBus().Unsubscribe(Id) ? 1u : 0u;
}

LUMINA_DOTNET_EXPORT(void, MessageBus_UnsubscribeAll)(uint64 World, uint32 Owner)
{
    if (World != 0)
    {
        AsWorld(World)->GetMessageBus().UnsubscribeAll(ECS::FEntity::FromPacked(Owner));
    }
}

// A managed payload's handle belongs to the bus once sent, whether or not anything receives it.
LUMINA_DOTNET_EXPORT(void, MessageBus_Send)(uint64 World, uint32 Route, uint32 Entity, uint32 Channel, uint32 Kind, uint64 Type,
                                            const void* Data, uint32 Size, const void* Struct, uint32 bIncludeSelf)
{
    FGameplayMessagePayloadRef Payload;
    Payload.Type   = Type;
    Payload.Data   = Data;
    Payload.Size   = Size;
    Payload.Kind   = (EGameplayMessagePayload)Kind;
    Payload.Struct = static_cast<const CStruct*>(Struct);

    if (World == 0 || Route > (uint32)EGameplayMessageRoute::To)
    {
        if (Payload.Kind == EGameplayMessagePayload::Managed)
        {
            FreeManagedListener(const_cast<void*>(Data));
        }
        return;
    }
    AsWorld(World)->GetMessageBus().Send((EGameplayMessageRoute)Route, ECS::FEntity::FromPacked(Entity), Channel, Payload, bIncludeSelf != 0);
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(MessageBus_TypeKey),
    LUMINA_DOTNET_SIG(MessageBus_Subscribe),
    LUMINA_DOTNET_SIG(MessageBus_Unsubscribe),
    LUMINA_DOTNET_SIG(MessageBus_UnsubscribeAll),
    LUMINA_DOTNET_SIG(MessageBus_Send)
);
