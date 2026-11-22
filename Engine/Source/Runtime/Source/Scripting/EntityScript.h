#pragma once

#include "World/ECS/Registry.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "ScriptReloadContext.h"
#include "World/Entity/Components/MissingComponents.h"
#include "EntityScript.generated.h"

namespace Lumina
{
    class CWorld;

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

        // Once, as the script joins its entity with owner and component references set, where events are bound and a sibling added this frame may still be missing.
        FUNCTION()
        virtual void OnAttach() {}

        // Once, on the first tick after attach, when every script and component added the same frame exists.
        FUNCTION()
        virtual void OnReady() {}

        // Every frame once ready, in ascending UpdatePriority, before physics unless its class sets UpdatePhase = PostPhysics.
        FUNCTION()
        virtual void OnUpdate(float DeltaTime) {}

        // At the fixed physics rate once ready, which may run zero or several times in one frame.
        FUNCTION()
        virtual void OnFixedUpdate(float FixedDeltaTime) {}

        // Each editor frame for every script in a world an editor viewport shows, attached or not, so authored values can be drawn with World.Debug.
        FUNCTION()
        virtual void OnDebugDraw(bool bSelected) {}

        // Once, before the script leaves its entity, which every bound member handler of this script does right after it returns.
        FUNCTION()
        virtual void OnDetach() {}

        /// The script load context was replaced, so anything bound in OnAttach has to be bound again. No other lifecycle event runs for a reload.
        FUNCTION()
        virtual void OnReloaded(SScriptReloadContext Context) {}

        // The level changed and SPersistentComponent carried this script along, so anything found in the old level is gone, listeners on its components too.
        FUNCTION()
        virtual void OnTraveled() {}
        
        // The entity this script is attached to, valid from OnAttach on.
        FUNCTION()
        ECS::FEntity GetOwningEntity() const { return OwningEntity; }

        // The world this script's entity lives in, or null for a bare registry with no world.
        FUNCTION()
        CWorld* GetWorld() const { return OwningWorld; }

        FUNCTION()
        int32 GetUpdatePriority() const { return UpdatePriority; }

        // Lower runs first among the entity's scripts, a change after attach reordering them from the next pass on.
        FUNCTION()
        void SetUpdatePriority(int32 InPriority);

    private:

        // The driver and persistence reach what follows through it, so none of it is script API.
        friend struct FEntityScriptAccess;

        // The managed boundary caught a throw from OnAttach or OnReady, and only it knows which script raised it.
        FUNCTION(ScriptInternal)
        void MarkFaulted();

        // Set once by the driver at attach, before OnAttach runs.
        void SetOwner(ECS::FEntity InEntity, CWorld* InWorld)
        {
            OwningEntity = InEntity;
            OwningWorld  = InWorld;
        }

        // Points every PROPERTY(Component) reference at the owner's component, adding it unless the property says Component = Get.
        void BindComponentRefs();

        bool ShouldTick() const { return bReady && !bFaulted; }

        // OnReloaded is what a reload delivers, so the replacement carries on where the original was rather than attaching again.
        void OnReinstanced(CObject& Previous) override;

        ECS::FEntity OwningEntity = ECS::NullEntity;
        CWorld*      OwningWorld = nullptr;

        // The batched update's copy of this script's managed handle, trusted only while the generation still matches.
        void*  CachedManagedHandle     = nullptr;
        uint32 CachedHandleGeneration  = 0;

        // Every member-function binding this script made, ended at detach so none of them outlives it.
        FBindingToken* BindingToken = nullptr;

        // OnReady has run. Not serialized, so a loaded script readies again on its first tick.
        bool bReady = false;
        bool bFaulted = false;
        bool bBindingsEnded = false;

        int32 UpdatePriority = 0;
    };

    // The driver's way into a script's lifecycle state, so CEntityScript's public surface stays what a script author uses.
    struct FEntityScriptAccess
    {
        static void SetOwner(CEntityScript& Script, ECS::FEntity Entity, CWorld* World) { Script.SetOwner(Entity, World); }
        static void BindComponentRefs(CEntityScript& Script) { Script.BindComponentRefs(); }
        // OnAttach has run, so a script loaded but never adopted gets no OnDetach.
        static bool IsAttached(const CEntityScript& Script) { return Script.OwningEntity != ECS::NullEntity; }

        static bool IsReady(const CEntityScript& Script) { return Script.bReady; }

        // Set when OnAttach or OnReady threw, so a half-built script keeps its OnDetach but never ticks.
        static bool IsFaulted(const CEntityScript& Script) { return Script.bFaulted; }

        static void MarkReady(CEntityScript& Script) { Script.bReady = true; }
        static void MarkFaulted(CEntityScript& Script) { Script.MarkFaulted(); }
        static bool ShouldTick(const CEntityScript& Script) { return Script.ShouldTick(); }

        static void*&  CachedManagedHandle(CEntityScript& Script) { return Script.CachedManagedHandle; }
        static uint32& CachedHandleGeneration(CEntityScript& Script) { return Script.CachedHandleGeneration; }

        RUNTIME_API static FBindingToken* BindingToken(CEntityScript& Script);
        RUNTIME_API static void EndBindings(CEntityScript& Script);
    };

    // Found by TScriptDelegate::AddMember through ADL, so a script's member handlers unbind themselves when it detaches.
    inline FBindingToken* BindingTokenFor(CEntityScript* Script)
    {
        return Script != nullptr ? FEntityScriptAccess::BindingToken(*Script) : nullptr;
    }

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

        #if USING(WITH_EDITOR)
        // Runs OnDebugDraw on every script in the registry's world, lending one the editor never attached its owner for the call.
        RUNTIME_API void DebugDraw(ECS::FRegistry& Registry);
        #endif

        // Drains pending OnReady (PrePhysics only), then runs OnUpdate on the scripts declaring Phase.
        RUNTIME_API void Tick(ECS::FRegistry& Registry, float DeltaTime,
            EScriptUpdatePhase Phase = EScriptUpdatePhase::PrePhysics);

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

    }
}
