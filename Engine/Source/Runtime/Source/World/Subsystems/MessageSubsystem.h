#pragma once

#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Memory/SmartPtr.h"
#include "Scripting/ScriptCallback.h"
#include "World/ECS/Entity.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "MessageSubsystem.generated.h"

namespace Lumina
{
    class FGameplayMessageBus;

    // The world's gameplay message bus and event queue access, with typed sending and listening for C# built over these.
    REFLECT()
    class RUNTIME_API CMessageSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        CMessageSubsystem();
        ~CMessageSubsystem() override;

        // The key C++ and C# senders share for a message type name.
        FUNCTION(ScriptInternal)
        static uint64 GetMessageTypeKey(FName TypeName);

        // The bus owns Listener from here, and releases it when the subscription goes.
        FUNCTION(ScriptInternal)
        uint64 Subscribe(uint32 Channel, ECS::FEntity Owner, uint64 Type, bool bExactMatch, SScriptCallback Listener);

        FUNCTION(ScriptInternal)
        bool Unsubscribe(uint64 Id);

        // Drops every listener Owner registered, which the bus also does on its own when the entity dies.
        FUNCTION()
        void UnsubscribeAll(ECS::FEntity Owner);

        // A managed payload's handle belongs to the bus once sent, whether or not anything receives it.
        FUNCTION(ScriptInternal)
        void Send(uint32 Route, ECS::FEntity Target, uint32 Channel, uint32 Kind, uint64 Type,
            void* Data, uint32 Size, void* Struct, bool bIncludeSelf);

        // The queue type for a reflected event of that name, or a script event laid out as described. 0xFFFF when neither fits.
        FUNCTION(ScriptInternal)
        static uint32 ResolveEventType(FName Name, uint32 Size, uint32 Alignment);

        FUNCTION(ScriptInternal)
        void SendEvents(uint32 TypeId, void* Events, uint32 Count);

        // Fills Runs, laid out as ECS::FEventRuns, with what is unread past the uint64 at Cursor, and moves the cursor past it.
        FUNCTION(ScriptInternal)
        void ReadEvents(uint32 TypeId, void* Cursor, void* Runs);

        FUNCTION(ScriptInternal)
        uint32 NumUnreadEvents(uint32 TypeId, uint64 Cursor);

        void OnInitialize() override;
        void OnTeardown() override;
        void OnDestroy() override;

        // The native bus, for typed C++ sends and subscriptions.
        FGameplayMessageBus& GetBus() { return *Bus; }

    private:

        // Built in OnInitialize, since it records deferred sends against the world's command bus.
        TUniquePtr<FGameplayMessageBus> Bus;
    };
}
