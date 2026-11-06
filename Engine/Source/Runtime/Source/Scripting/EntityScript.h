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
#include "World/Entity/Components/MissingComponents.h"
#include "EntityScript.generated.h"

// The first statement of a FUNCTION(Rpc = ...) body, passed the function's parameters in order, the C++ form of calling a C# [Rpc] method.
#define NET_RPC(...) do { if (this->RouteRpc(__func__ __VA_OPT__(,) __VA_ARGS__)) { return; } } while (false)

namespace Lumina
{
    class CWorld;
    class FFunction;
    class FProperty;
    class FPropertySnapshot;

    // Where one routed call runs, decided the same way for NET_RPC and a C# [Rpc] method.
    REFLECT()
    enum class ERpcRoute : uint8
    {
        RunHere,
        SendAndRunHere,
        SendOnly,
    };

    REFLECT()
    enum class EScriptUpdatePhase : uint8
    {
        PrePhysics = 0,
        PostPhysics = 1,
    };

    /// Base for a script attached to one entity, whose scripts run every pass in ascending UpdatePriority, ties in attach order, C++ and C# alike on the parallel and serial paths.
    REFLECT(Scriptable)
    class RUNTIME_API CEntityScript : public CObject
    {
        GENERATED_BODY()

    public:

        ~CEntityScript() override;
        CEntityScript() = default;
        CEntityScript(const CEntityScript&) = delete;
        CEntityScript& operator=(const CEntityScript&) = delete;
        CEntityScript(CEntityScript&&) = delete;
        CEntityScript& operator=(CEntityScript&&) = delete;

        FUNCTION()
        virtual void OnAttach() {}

        FUNCTION()
        virtual void OnReady() {}

        FUNCTION()
        virtual void OnUpdate(float DeltaTime) {}

        FUNCTION()
        virtual void OnDebugDraw(bool bSelected) {}

        // Once a frame while the developer overlay shows, between ImGui's NewFrame and Render, so ImGui calls build debug windows.
        FUNCTION()
        virtual void OnDebugGui() {}

        FUNCTION()
        virtual void OnFixedUpdate(float FixedDeltaTime) {}

        FUNCTION()
        virtual void OnDetach() {}

        /// The script load context was replaced, so anything bound in OnAttach has to be bound again. No other lifecycle event runs for a reload.
        FUNCTION()
        virtual void OnReloaded(SScriptReloadContext Context) {}

        /// One input event, delivered only to an entity with an SInputComponent while its viewport has game input focus.
        FUNCTION()
        virtual void OnInput(SInputEvent Event) {}

        /// An authored action that changed this frame. A C# override must call base, which feeds its input bindings.
        FUNCTION()
        virtual void OnAction(FName Action, SInputActionState State) {}

        // The level changed and SPersistentComponent carried this script along, so anything found in the old level is gone.
        FUNCTION()
        virtual void OnTraveled() {}

        // A save game just wrote this running script's SaveGame fields back, so anything built from them is stale.
        FUNCTION()
        virtual void OnRestored() {}

        // A save game is about to read this script's SaveGame fields, so derived state can be folded into them.
        FUNCTION()
        virtual void OnSaving() {}

        /// The entity this script is attached to, valid from OnAttach on.
        FUNCTION()
        ECS::FEntity GetOwningEntity() const { return OwningEntity; }

        /// The world this script's entity lives in, or null for a bare registry with no world.
        FUNCTION()
        CWorld* GetWorld() const { return OwningWorld; }

        // Set once by the driver at attach, before OnAttach runs.
        void SetOwner(ECS::FEntity InEntity, CWorld* InWorld)
        {
            OwningEntity = InEntity;
            OwningWorld  = InWorld;
        }

        // Points every PROPERTY(Component) reference at the owner's component, adding it unless the property says Component = Get.
        void BindComponentRefs();

        /// OnAttach has run, so a script loaded but never adopted gets no OnDetach.
        bool IsAttached() const { return OwningEntity != ECS::NullEntity; }

        FUNCTION()
        bool IsReady() const { return bReady; }
        void MarkReady() { bReady = true; }

        // Set when OnAttach or OnReady threw, so a half-built script keeps its OnDetach but never ticks.
        FUNCTION()
        bool IsFaulted() const { return bFaulted; }

        // The managed boundary caught a throw from OnAttach or OnReady, and only it knows which script raised it.
        FUNCTION(ScriptInternal)
        void MarkFaulted();

        bool ShouldTick() const { return bReady && !bFaulted; }

        FUNCTION()
        int32 GetUpdatePriority() const { return UpdatePriority; }

        // Lower runs first among the entity's scripts, a change after attach reordering them from the next pass on.
        FUNCTION()
        void SetUpdatePriority(int32 InPriority);

        // OnReloaded is what a reload delivers, so the replacement carries on where the original was rather than attaching again.
        void OnReinstanced(CObject& Previous) override;

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
        FUNCTION(ScriptInternal)
        static bool IsReceivingRpc();

        // The connection that sent what is being applied, or zero when nothing is.
        FUNCTION(ScriptInternal)
        static uint32 GetReceivingCaller();

        // Resends the entity's replicated component fields. A PROPERTY(Sync) field needs no call, since it is checked every tick.
        FUNCTION(ScriptInternal)
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

        //~ The C# [Rpc] and [Property(Sync)] runtime, which finds a function or field once and then addresses it by pointer.

        // The reflected RPC named Name, or null when this script has none by that name.
        FUNCTION(ScriptInternal)
        void* FindRpcFunction(FName Name) const;

        // PlanRpc for a function FindRpcFunction returned.
        FUNCTION(ScriptInternal)
        ERpcRoute PlanRpcCall(void* Function);

        // A default call frame the caller fills slot by slot and hands to SendRpcCall, which frees it.
        FUNCTION(ScriptInternal)
        static void* MakeRpcFrame(void* Function);

        FUNCTION(ScriptInternal)
        void SendRpcCall(void* Function, void* Frame);

        // Frees a frame from MakeRpcFrame that will never be sent, such as one whose arguments failed to write.
        FUNCTION(ScriptInternal)
        static void DiscardRpcFrame(void* Function, void* Frame);

        // The FromOwner Sync field named Name, or null when this script has none by that name.
        FUNCTION(ScriptInternal)
        void* FindSyncField(FName Name) const;

        // Sends an owner's write of Field to the host, which validates and applies it as it would a C++ one.
        FUNCTION(ScriptInternal)
        void ForwardSyncField(void* Field);

        // An RPC or an owner's Sync write arrived for this script, read through the same FProperty::NetSerialize it was written with.
        void ReceiveRpc(uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize);

        //~ PROPERTY(Sync) support, used by the netcode, which checks Sync fields every tick the way a C# Sync setter would.

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

        // Compares a C++ script's Sync fields with the values last seen, doing what a C# Sync setter does for each change.
        void PollSync();

        // Replicated values are about to land, or just landed, in the named fields, which are zero-terminated back to back.
        void SyncArriving();
        void SyncArrived(const char* Names, uint32 NamesSize);

        //~ Session events, which a C# script overrides like any other script event.

        // Host. A client finished joining.
        FUNCTION()
        virtual void OnConnected(uint32 ConnectionId) {}

        // Host. A client left, told before anything it owned changes hands.
        FUNCTION()
        virtual void OnDisconnected(uint32 ConnectionId) {}

        // Client. This peer finished joining its host.
        FUNCTION()
        virtual void OnJoinedHost() {}

        // Client. The session ended, or never came up, for Reason.
        FUNCTION()
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

        int32 UpdatePriority = 0;

        // The Sync values PollSync last saw, built on first use and only for a C++ script.
        FPropertySnapshot* SyncSnapshot = nullptr;

        void ReceiveSync(FProperty* Field, bool bCorrection, uint32 CallerId, const uint8* Payload, uint32 PayloadSize);
        void ApplySync(FProperty* Field, const void* Value);
        FPropertySnapshot& EnsureSyncSnapshot();
    };

    /// Holds the scripts attached to one entity, each a CEntityScript of any class, native or minted from C#.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SEntityScriptComponent
    {
        GENERATED_BODY()

        SEntityScriptComponent() = default;
        ~SEntityScriptComponent();
        SEntityScriptComponent(SEntityScriptComponent&&) = default;
        SEntityScriptComponent& operator=(SEntityScriptComponent&& Other) noexcept;

        // A script is a per-entity subobject, so a copy (prefab stamp, component duplicate) clones it.
        SEntityScriptComponent(const SEntityScriptComponent& Other);
        SEntityScriptComponent& operator=(const SEntityScriptComponent& Other);

        // Reflected so references can be walked and repointed, NoSerialize since the component serializes itself.
        PROPERTY(Hidden, NoSerialize)
        TVector<TStrongObjectPtr<CEntityScript>> Scripts;

        // Unresolved at load, written back with what its bytes refer to, retried on the next script reload.
        TVector<SHeldValue> Pending;

        bool Serialize(FArchive& Ar);
    };

    /// The script driver, which is the same for a C++ script and a C# one.
    namespace EntityScripts
    {
        /// Creates a script on Entity and runs OnAttach, OnReady waiting for the first tick so every sibling exists by then.
        RUNTIME_API CEntityScript* Attach(ECS::FRegistry& Registry, ECS::FEntity Entity, CClass* ScriptClass);

        // A detached copy of Source's values with no owner and no lifecycle, for script data held outside a world.
        RUNTIME_API CEntityScript* Clone(CEntityScript* Source);

        // Drains pending OnReady (PrePhysics only), then runs OnUpdate on the scripts declaring Phase.
        RUNTIME_API void Tick(ECS::FRegistry& Registry, float DeltaTime,
            EScriptUpdatePhase Phase = EScriptUpdatePhase::PrePhysics);

        #if USING(WITH_EDITOR)
        RUNTIME_API void DebugDraw(ECS::FRegistry& Registry);
        #endif

        #if USING(WITH_DEV_UI)
        // Runs OnDebugGui on every live script, recovering ImGui's stacks after each so one that throws or forgets End leaves the rest intact.
        RUNTIME_API void DebugGui(ECS::FRegistry& Registry);
        #endif

        // Bumped by anything that changes which scripts are attached or able to tick, so a batched update knows when to stop.
        RUNTIME_API void NoteStructureChange();

        // Report, once per class and world, a ParallelUpdate script in the parallel pass on this thread that broke its promise through Call.
#if defined(LE_SHIPPING)
        inline void CheckParallelAccess(ECS::FEntity, const char*) {}
        inline void CheckParallelStructureChange(const char*) {}
#else
        RUNTIME_API void CheckParallelAccess(ECS::FEntity Entity, const char* Call);
        RUNTIME_API void CheckParallelStructureChange(const char* Call);
#endif

        // Runs OnFixedUpdate on every ready script. Driven at the physics rate.
        RUNTIME_API void TickFixed(ECS::FRegistry& Registry, float FixedDeltaTime);

        // Runs OnDetach and drops every script on Entity.
        RUNTIME_API void DetachAll(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Walks a snapshot, so an OnDetach that adds or removes scripts cannot invalidate the pool underneath.
        RUNTIME_API void DetachAllInRegistry(ECS::FRegistry& Registry);

        // Lookup and mutation by class, which finds a C++ script the same way as a C# one.

        // The first script on Entity whose class is or derives from ScriptClass, or null.
        RUNTIME_API CEntityScript* Find(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass);

        // Appends every script on Entity whose class is or derives from ScriptClass.
        RUNTIME_API void FindAll(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out);

        // Runs OnDetach on Script and removes it from its entity. Returns false if it was not attached.
        RUNTIME_API bool Remove(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script);

        /// Delivers OnReloaded to every C# script in every world, a C++ script having nothing to reload.
        RUNTIME_API void NotifyScriptsReloaded(EScriptReloadReason Reason, int32 Generation);

        // Materializes scripts held back at load because their class was missing, returning the count.
        RUNTIME_API int32 ResolvePendingScripts(ECS::FRegistry& Registry);

        // Delivers one input event to every script on Entity.
        RUNTIME_API void DispatchInput(ECS::FRegistry& Registry, ECS::FEntity Entity, const SInputEvent& Event);

        // Delivers OnAction for each of ChangedActionIndices to every script on Entity.
        RUNTIME_API void DispatchActions(ECS::FRegistry& Registry, ECS::FEntity Entity, const SInputActionState* States,
            int32 Count, TSpan<const int32> ChangedActionIndices);

    }
}
