#include "RuntimePCH.h"
#include "World/Subsystems/MessageSubsystem.h"

#include "Core/Object/Class.h"
#include "HierarchicalTags/GameplayMessageBus.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        // The context is the listener's callback token, and the message crosses as its address.
        uint32 InvokeManagedListener(void* Context, const FGameplayMessage* Message)
        {
            return Scripting::InvokeScriptCallbackRepeating(SScriptCallback{ reinterpret_cast<uint64>(Context) }, reinterpret_cast<uint64>(Message)) != 0 ? 1u : 0u;
        }
    }

    CMessageSubsystem::CMessageSubsystem() = default;
    CMessageSubsystem::~CMessageSubsystem() = default;

    void CMessageSubsystem::OnInitialize()
    {
        CWorld* World = GetWorld();
        Bus = MakeUnique<FGameplayMessageBus>(World->GetEntityRegistry(), World->GetCommandBus());
    }

    void CMessageSubsystem::OnTeardown()
    {
        if (Bus)
        {
            Bus->Clear();
            Bus.reset();
        }
    }

    void CMessageSubsystem::OnDestroy()
    {
        Super::OnDestroy();

        // Released before any object is freed, since a world never torn down frees its registry before this subsystem.
        Bus.reset();
    }

    uint64 CMessageSubsystem::GetMessageTypeKey(FName TypeName)
    {
        return TypeName.IsNone() ? 0 : MakeGameplayMessageType(TypeName);
    }

    uint64 CMessageSubsystem::Subscribe(uint32 Channel, ECS::FEntity Owner, uint64 Type, bool bExactMatch, SScriptCallback Listener)
    {
        if (!Listener.IsBound())
        {
            Scripting::ReleaseScriptCallback(Listener);
            return 0;
        }

        FGameplayMessageBus::FSubscribeParams Params;
        Params.Channel  = Channel;
        Params.Owner    = Owner;
        Params.Type     = Type;
        Params.Match    = bExactMatch ? EHierarchicalTagMatch::Exact : EHierarchicalTagMatch::Partial;
        Params.Invoke   = &InvokeManagedListener;
        Params.Context  = reinterpret_cast<void*>(Listener.Token);
        Params.Destroy  = &DotNet::FreeManagedHandle;
        Params.bManaged = true;
        return Bus->Subscribe(Params);
    }

    bool CMessageSubsystem::Unsubscribe(uint64 Id)
    {
        return Bus->Unsubscribe(Id);
    }

    void CMessageSubsystem::UnsubscribeAll(ECS::FEntity Owner)
    {
        Bus->UnsubscribeAll(Owner);
    }

    void CMessageSubsystem::Send(uint32 Route, ECS::FEntity Target, uint32 Channel, uint32 Kind, uint64 Type,
        void* Data, uint32 Size, void* Struct, bool bIncludeSelf)
    {
        FGameplayMessagePayloadRef Payload;
        Payload.Type   = Type;
        Payload.Data   = Data;
        Payload.Size   = Size;
        Payload.Kind   = (EGameplayMessagePayload)Kind;
        Payload.Struct = static_cast<const CStruct*>(Struct);

        if (Route > (uint32)EGameplayMessageRoute::To)
        {
            if (Payload.Kind == EGameplayMessagePayload::Managed)
            {
                DotNet::FreeManagedHandle(Data);
            }
            return;
        }
        Bus->Send((EGameplayMessageRoute)Route, Target, Channel, Payload, bIncludeSelf);
    }
}
