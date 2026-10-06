#pragma once

#include "World/ECS/Registry.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/InputAction.h"
#include "Input/InputEvent.h"
#include "Networking/NetworkTypes.h"
#include "ScriptReloadContext.h"
#include "EntityScript.generated.h"

// The first statement of a FUNCTION(Rpc = ...) body, passed the function's parameters in order, the C++ form of calling a C# [Rpc] method.
#define NET_RPC(...) do { if (this->RouteRpc(__func__ __VA_OPT__(,) __VA_ARGS__)) { return; } } while (false)

namespace Lumina
{
    class CWorld;
    class FFunction;
    class FProperty;
    struct FSyncSnapshot;

    // Where one routed call runs, decided the same way for NET_RPC and a C# [Rpc] method.
    enum class ERpcRoute : uint8
    {
        RunHere,
        SendAndRunHere,
        SendOnly,
    };

    // Which side of the physics step a script's OnUpdate runs on. Mirrors LuminaSharp.EScriptPhase.
    enum class EScriptUpdatePhase : uint8
    {
        PrePhysics = 0,
        PostPhysics = 1,
    };

    /**
     * Base for a script attached to a single entity.
     */
    REFLECT(Scriptable)
    class RUNTIME_API CEntityScript : public CObject
    {
        GENERATED_BODY()

    public:

        ~CEntityScript() override;

        FUNCTION()
        virtual void OnAttach() {}

        FUNCTION()
        virtual void OnReady() {}

        FUNCTION()
        virtual void OnUpdate(float DeltaTime) {}

        FUNCTION()
        virtual void OnFixedUpdate(float FixedDeltaTime) {}

        FUNCTION()
        virtual void OnDetach() {}

        /** The script load context was replaced. Nothing that lived in the old one survived, so anything
         *  bound in OnAttach is unbound here and has to be bound again. OnAttach, OnReady and OnDetach do
         *  not run for a reload; this is the whole of it. */
        FUNCTION()
        virtual void OnReloaded(SScriptReloadContext Context) {}

        /** One discrete input event (key/mouse press, move, scroll). Delivered only to entities carrying an
         *  SInputComponent, and only while their viewport has game input focus -- see SInputSystem. */
        FUNCTION()
        virtual void OnInput(SInputEvent Event) {}

        /** An authored action that changed this frame: pressed, released, or an axis off zero. A C# script
         *  overriding this must call base, which is what feeds its SInputAction / SInputAxis bindings. */
        FUNCTION()
        virtual void OnAction(FName Action, FInputActionState State) {}

        /** The entity this script is attached to. Valid from OnAttach onwards.
         *  FUNCTION() so the C# base reads its entity from here rather than being handed one separately --
         *  one owner for the value in both languages. Non-virtual, so it binds as an ordinary call, not a
         *  ScriptEvent. */
        FUNCTION()
        ECS::FEntity GetOwningEntity() const { return OwningEntity; }

        /** The world this script's entity lives in, or null when the registry has no world (a bare registry
         *  in a test). Resolved once at attach from the registry's CWorld* context singleton. */
        FUNCTION()
        CWorld* GetWorld() const { return OwningWorld; }

        /** Set once by the driver at attach, before OnAttach runs. */
        void SetOwner(ECS::FEntity InEntity, CWorld* InWorld)
        {
            OwningEntity = InEntity;
            OwningWorld  = InWorld;
        }

        /** OnAttach has run. The driver sets the owner immediately before it, so this is the exact pairing
         *  test: a script that was loaded or stamped but never adopted must not receive OnDetach. */
        bool IsAttached() const { return OwningEntity != ECS::NullEntity; }

        FUNCTION()
        bool IsReady() const { return bReady; }
        void MarkReady() { bReady = true; }

        // Set when OnAttach or OnReady threw, so a half-built script keeps its OnDetach but never ticks.
        FUNCTION()
        bool IsFaulted() const { return bFaulted; }
        void MarkFaulted();

        bool ShouldTick() const { return bReady && !bFaulted; }

        //~ Networking, spelled the way LuminaSharp's Networking and EntityNetwork are. A standalone world answers as its own host.

        // True on the host, a dedicated server and a standalone world, the peers whose word is final.
        bool IsHost() const;

        // True when this peer controls the entity, which a standalone world and an unnetworked entity always do.
        bool IsOwner() const;

        // Another peer controls the entity, so this one only mirrors it.
        bool IsProxy() const { return !IsOwner(); }

        // The connection the RPC being handled came from, or this peer's own when the call was not received.
        uint32 GetRpcCaller() const;

        // True on this thread while a received RPC or an owner's Sync write is being applied.
        static bool IsReceivingRpc();

        // The connection that sent what is being applied, or zero when nothing is.
        static uint32 GetReceivingCaller();

        // Resends the entity's replicated component fields. A PROPERTY(Sync) field needs no call, since it is checked every tick.
        void MarkNetDirty();

        // Called through NET_RPC. Sends the call where its FUNCTION(Rpc = ...) says and returns true when it should not also run here.
        template<typename... TArgs>
        bool RouteRpc(const char* FunctionName, const TArgs&... Args)
        {
            const void* Values[] = { static_cast<const void*>(&Args)..., nullptr };
            return RouteRpcValues(FunctionName, Values, static_cast<uint32>(sizeof...(TArgs)));
        }

        bool RouteRpcValues(const char* FunctionName, const void* const* Values, uint32 NumValues);

        // Where a call to Function goes from here. Consumes the mark a received call leaves, so the arriving call runs locally.
        ERpcRoute PlanRpc(const FFunction& Function);

        // Writes each argument through its parameter's FProperty::NetSerialize and sends the call, Values in parameter order.
        void SendRpc(const FFunction& Function, const void* const* Values);

        // The same, reading the arguments out of a call frame laid out for Function.
        void SendRpcFrame(const FFunction& Function, const void* Frame);

        // An RPC or an owner's Sync write arrived for this script, read through the same FProperty::NetSerialize it was written with.
        void ReceiveRpc(uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize);

        //~ PROPERTY(Sync) support, used by the netcode, which checks Sync fields every tick the way a C# [Sync] setter would.

        // The id an owner's write to a FromOwner field travels under, or the one the host's correction comes back under.
        uint32 SyncFieldId(const FProperty* Field, bool bCorrection) const;

        // Runs the field's PROPERTY(Change = ...) function, if it names one, with OldValue and the current value.
        void NotifySyncChanged(const FProperty* Field, const void* OldValue);

        // Whether the field's PROPERTY(Validate = ...) function accepts Proposed. A field without one accepts anything.
        bool ValidateSync(const FProperty* Field, const void* Proposed);

        // Snaps a numeric field to its PROPERTY(Quantize = ...) step in place.
        void QuantizeSync(const FProperty* Field);

        // Sends the field's current value to the host as an owner's write, or back to the owner as a correction.
        void SendSync(const FProperty* Field, bool bCorrection);

        // Whether the field is PROPERTY(Sync = FromOwner), the only kind an owner may write.
        static bool IsSyncFromOwner(const FProperty* Field);

        // Compares a C++ script's Sync fields with the values last seen, doing what a C# [Sync] setter does for each change.
        void PollSync();

        // Replicated values are about to land, or just landed, in the named fields, which are zero-terminated back to back.
        void SyncArriving();
        void SyncArrived(const char* Names, uint32 NamesSize);

        //~ Session events, the C++ side of LuminaSharp.INetworkListener.

        // Host. A client finished joining.
        virtual void OnConnected(uint32 ConnectionId) {}

        // Host. A client left, told before anything it owned changes hands.
        virtual void OnDisconnected(uint32 ConnectionId) {}

        // Client. This peer finished joining its host.
        virtual void OnJoinedHost() {}

        // Client. The session ended, or never came up, for Reason.
        virtual void OnLeftHost(ENetLeaveReason Reason) {}

        // The batched update's copy of this script's managed handle, trusted only while the generation still matches.
        void*  CachedManagedHandle     = nullptr;
        uint32 CachedHandleGeneration  = 0;


    private:

        ECS::FEntity OwningEntity = ECS::NullEntity;
        CWorld*      OwningWorld = nullptr;

        // Transient: OnReady has run. Not serialized -- a loaded script re-readies on its first tick.
        bool bReady = false;
        bool bFaulted = false;

        // The Sync values PollSync last saw, built on first use and only for a C++ script.
        FSyncSnapshot* SyncSnapshot = nullptr;

        void ReceiveSync(FProperty* Field, bool bCorrection, uint32 CallerId, const uint8* Payload, uint32 PayloadSize);
        void ApplySync(FProperty* Field, const void* Value);
        FSyncSnapshot& EnsureSyncSnapshot();
    };

    /** One script kept verbatim because its class was not loadable when the world was read. */
    struct FPendingScript
    {
        FName          ClassName;
        TVector<uint8> Bytes;

        /** Replayed on restore so the held bytes are read exactly as the file wrote them. */
        int32          FileVersion = 0;

        // A package writes names and objects as indices into its own tables, so plain bytes cannot be read back alone.
        FArchive::FDeferredReaderFactory MakeReader;
    };

    /** Holds the scripts attached to one entity. Language-agnostic: each element is a CEntityScript of
     *  whatever CClass, native or minted-from-C#. */
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SEntityScriptComponent
    {
        GENERATED_BODY()

        SEntityScriptComponent() = default;
        ~SEntityScriptComponent();
        SEntityScriptComponent(SEntityScriptComponent&&) = default;
        SEntityScriptComponent& operator=(SEntityScriptComponent&& Other);

        // A script is a per-entity subobject, so a copy (prefab stamp, component duplicate) clones it.
        SEntityScriptComponent(const SEntityScriptComponent& Other);
        SEntityScriptComponent& operator=(const SEntityScriptComponent& Other);

        // Reflected so the object graph can be walked and repointed, NoSerialize because the component
        // carries its own Serialize and a second, per-property path would fight it.
        PROPERTY(NoSerialize)
        TVector<TStrongObjectPtr<CEntityScript>> Scripts;

        /** Unresolved at load, written back out untouched, retried on the next script reload. */
        TVector<FPendingScript> Pending;

        bool Serialize(FArchive& Ar);
    };

    /**
     * The whole script driver. Every function here is language-agnostic on purpose: adding C++ scripts cost
     * nothing beyond this file existing, and adding a third language would cost nothing here either.
     */
    namespace EntityScripts
    {
        /** Creates a script of ScriptClass on Entity, runs OnAttach, and returns it (null if the class is not
         *  a CEntityScript). OnReady is deferred to the first Tick, so a script can rely on every sibling
         *  script on the entity existing by the time it runs. */
        RUNTIME_API CEntityScript* Attach(ECS::FRegistry& Registry, ECS::FEntity Entity, CClass* ScriptClass);

        /** Drains pending OnReady (PrePhysics only), then runs OnUpdate on the scripts declaring Phase. */
        RUNTIME_API void Tick(ECS::FRegistry& Registry, float DeltaTime,
            EScriptUpdatePhase Phase = EScriptUpdatePhase::PrePhysics);

        // Bumped by anything that changes which scripts are attached or able to tick, so a batched update knows when to stop.
        RUNTIME_API void NoteStructureChange();

        /** Runs OnFixedUpdate on every ready script. Driven at the physics rate. */
        RUNTIME_API void TickFixed(ECS::FRegistry& Registry, float FixedDeltaTime);

        /** Runs OnDetach and drops every script on Entity. */
        RUNTIME_API void DetachAll(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Walks a snapshot, so an OnDetach that adds or removes scripts cannot invalidate the pool underneath.
        RUNTIME_API void DetachAllInRegistry(ECS::FRegistry& Registry);

        //~ Lookup/mutation by class, backing the script-facing GetScript/AddScript/RemoveScript API. Class
        //~ rather than C# type: a C++ script is found by exactly the same call.

        /** The first script on Entity whose class IS-A ScriptClass, or null. */
        RUNTIME_API CEntityScript* Find(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass);

        /** Appends every script on Entity whose class IS-A ScriptClass. */
        RUNTIME_API void FindAll(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out);

        /** Runs OnDetach on Script and removes it from its entity. Returns false if it was not attached. */
        RUNTIME_API bool Remove(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script);

        /** Delivers OnReloaded to every C#-backed script in every world. A C++ script is not reloaded, so
         *  it is skipped rather than told about someone else's reload. */
        RUNTIME_API void NotifyScriptsReloaded(EScriptReloadReason Reason, int32 Generation);

        /** Materializes scripts held back at load because their class was missing, returning the count. */
        RUNTIME_API int32 ResolvePendingScripts(ECS::FRegistry& Registry);

        /** Delivers one input event to every script on Entity. */
        RUNTIME_API void DispatchInput(ECS::FRegistry& Registry, ECS::FEntity Entity, const SInputEvent& Event);

        /** Delivers OnAction for each of ChangedActionIndices to every script on Entity. */
        RUNTIME_API void DispatchActions(ECS::FRegistry& Registry, ECS::FEntity Entity, const FInputActionState* States,
            int32 Count, TSpan<const int32> ChangedActionIndices);

    }
}
