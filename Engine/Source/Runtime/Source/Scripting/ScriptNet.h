#pragma once

#include "Containers/Vector.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Reflection/PropertySnapshot.h"
#include "Memory/SmartPtr.h"
#include "Networking/NetworkTypes.h"
#include "World/ECS/Entity.h"
#include "World/ECS/EventMeta.h"
#include "ScriptNet.generated.h"

namespace Lumina
{
    class CEntityScript;
    class CWorld;
    class FFunction;
    class FProperty;
    struct SEntityScriptComponent;

    namespace ECS
    {
        class FRegistry;
    }

    // Where one routed call runs, decided the same way for a C++ RPC's generated override and a C# [Rpc] method.
    REFLECT()
    enum class ERpcRoute : uint8
    {
        RunHere,
        SendAndRunHere,
        SendOnly,
    };

    // An RPC, or an owner's write to a Sync field, that arrived for one script. The network system queues it and the sync point runs it.
    REFLECT(Event)
    struct RUNTIME_API SScriptRpcEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        uint32 ScriptIndex = 0;

        PROPERTY(Hidden)
        uint32 RpcId = 0;

        // The connection the call came from, 0 for the host's own word.
        PROPERTY(Hidden)
        uint32 CallerId = 0;

        PROPERTY(Hidden)
        TVector<uint8> Payload;
    };

    // Replicated values landed in one script's Sync fields, named zero-terminated back to back, so its change handlers run at the sync point.
    REFLECT(Event)
    struct RUNTIME_API SScriptSyncArrivedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        uint32 ScriptIndex = 0;

        PROPERTY(Hidden)
        TVector<uint8> Names;
    };

    template<>
    struct TEventFanOut<SScriptRpcEvent>
    {
        RUNTIME_API static void Deliver(ECS::FRegistry& Registry, const SScriptRpcEvent& Event);
    };

    template<>
    struct TEventFanOut<SScriptSyncArrivedEvent>
    {
        RUNTIME_API static void Deliver(ECS::FRegistry& Registry, const SScriptSyncArrivedEvent& Event);
    };

    // A networked entity's Sync fields as each script's change handlers last saw them. Copying drops the snapshots, which rebuild on use.
    struct RUNTIME_API FScriptSyncState
    {
        struct FEntry
        {
            const CEntityScript*          Script = nullptr;
            TUniquePtr<FPropertySnapshot> Snapshot;
        };

        FScriptSyncState() = default;
        FScriptSyncState(const FScriptSyncState&) {}
        FScriptSyncState& operator=(const FScriptSyncState&) { Entries.Reset(); return *this; }
        FScriptSyncState(FScriptSyncState&&) noexcept = default;
        FScriptSyncState& operator=(FScriptSyncState&&) noexcept = default;
        ~FScriptSyncState();

        // Made and captured on first use, so a script's first sighting is its baseline. A null snapshot means the class has no Sync field.
        FEntry& FindOrCapture(const CEntityScript& Script, bool& bOutCaptured);

        // Drops entries for scripts the entity no longer holds.
        void Prune(const SEntityScriptComponent& Scripts);

        TVector<FEntry> Entries;
    };

    // The C# [Rpc] and [Property(Sync)] runtime's way into native routing, so every function is internal to C#.
    REFLECT()
    class RUNTIME_API CScriptNetLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // True on this thread while a received RPC or an owner's Sync write is being applied.
        FUNCTION(ScriptInternal)
        static bool IsReceivingRpc();

        // The connection that sent what is being applied, or zero when nothing is.
        FUNCTION(ScriptInternal)
        static uint32 GetReceivingCaller();

        // The reflected RPC named Name, or null when the script has none by that name.
        FUNCTION(ScriptInternal)
        static void* FindRpcFunction(CEntityScript* Script, FName Name);

        FUNCTION(ScriptInternal)
        static ERpcRoute PlanRpcCall(CEntityScript* Script, void* Function);

        // A default call frame the caller fills slot by slot and hands to SendRpcCall, which frees it.
        FUNCTION(ScriptInternal)
        static void* MakeRpcFrame(void* Function);

        FUNCTION(ScriptInternal)
        static void SendRpcCall(CEntityScript* Script, void* Function, void* Frame);

        // Frees a frame from MakeRpcFrame that will never be sent, such as one whose arguments failed to write.
        FUNCTION(ScriptInternal)
        static void DiscardRpcFrame(void* Function, void* Frame);

        // The FromOwner Sync field named Name, or null when the script has none by that name.
        FUNCTION(ScriptInternal)
        static void* FindSyncField(CEntityScript* Script, FName Name);

        // Sends an owner's write of Field to the host, which validates and applies it as it would a C++ one.
        FUNCTION(ScriptInternal)
        static void ForwardSyncField(CEntityScript* Script, void* Field);
    };

    namespace ScriptNet
    {
        // Called by an RPC's generated override. Sends the call where its FUNCTION(Rpc = ...) says, true when it should not also run here.
        RUNTIME_API bool RouteRpc(CEntityScript& Script, const char* FunctionName, const void* const* Values, uint32 NumValues);

        // The target the script's own declaration gives RpcId, so a relay never trusts the one a client wrote.
        RUNTIME_API bool FindRpcTarget(const CEntityScript* Script, uint32 RpcId, ERpcTarget& OutTarget);

        // The script at Index in the entity's script component, or null.
        RUNTIME_API CEntityScript* ScriptAt(ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Index);

        // Called by the network system, which only queues. The script code an arrival triggers runs at the next sync point.
        RUNTIME_API void QueueRpc(CWorld& World, ECS::FEntity Entity, uint32 ScriptIndex, uint32 RpcId, uint32 CallerId, TVector<uint8> Payload);

        // Before replicated values overwrite a script's Sync fields, so the old values are kept for its change handlers.
        RUNTIME_API void BeginSyncArrival(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript& Script, const char* Names, uint32 NamesSize);
        RUNTIME_API void QueueSyncArrival(CWorld& World, ECS::FEntity Entity, uint32 ScriptIndex, const char* Names, uint32 NamesSize);

        // One entity's C++ Sync fields compared against their snapshots, sending or marking what this peer may write.
        RUNTIME_API void PollSync(CWorld& World, ECS::FEntity Entity, const SEntityScriptComponent& Scripts, FScriptSyncState& State);

        // The connection that sent the RPC or Sync write being applied on this thread, or zero when none is.
        RUNTIME_API bool IsReceiving();
        RUNTIME_API uint32 ReceivingCaller();
    }
}
