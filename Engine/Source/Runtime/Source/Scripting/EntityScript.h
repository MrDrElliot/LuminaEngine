#pragma once

#include "World/ECS/Registry.h"



#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/InputEvent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/Entity/Events/PerceptionEvent.h"
#include "EntityScript.generated.h"

namespace Lumina
{
    struct FInputActionState;

    class CWorld;

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

        /** One discrete input event (key/mouse press, move, scroll). Delivered only to entities carrying an
         *  SInputComponent, and only while their viewport has game input focus -- see SInputSystem. */
        FUNCTION()
        virtual void OnInput(SInputEvent Event) {}

        //~ Physics callbacks. Delivered by the physics scene's contact drain to every script on the entity,
        //~ so a C++ and a C# script receive them through the same virtual. The event is oriented per-entity
        //~ (Normal points away from self); both bodies get a callback with the roles swapped.

        FUNCTION()
        virtual void OnContactBegin(SCollisionEvent Event) {}

        FUNCTION()
        virtual void OnContactEnd(SCollisionEvent Event) {}

        FUNCTION()
        virtual void OnOverlapBegin(SCollisionEvent Event) {}

        FUNCTION()
        virtual void OnOverlapEnd(SCollisionEvent Event) {}

        //~ AI perception. Delivered to the PERCEIVER's scripts when one of its senses acquires or loses a
        //~ target, alongside the component delegate.

        FUNCTION()
        virtual void OnTargetPerceived(SPerceptionEvent Event) {}

        FUNCTION()
        virtual void OnTargetLost(SPerceptionEvent Event) {}

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

        bool IsReady() const { return bReady; }
        void MarkReady() { bReady = true; }

    private:

        ECS::FEntity OwningEntity = ECS::NullEntity;
        CWorld*      OwningWorld = nullptr;

        // Transient: OnReady has run. Not serialized -- a loaded script re-readies on its first tick.
        bool bReady = false;
    };

    /** Holds the scripts attached to one entity. Language-agnostic: each element is a CEntityScript of
     *  whatever CClass, native or minted-from-C#. */
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SEntityScriptComponent
    {
        GENERATED_BODY()

        SEntityScriptComponent() = default;
        ~SEntityScriptComponent() = default;
        SEntityScriptComponent(SEntityScriptComponent&&) = default;
        SEntityScriptComponent& operator=(SEntityScriptComponent&&) = default;

        // A script is a per-entity subobject, so a copy (prefab stamp, component duplicate) clones it.
        SEntityScriptComponent(const SEntityScriptComponent& Other);
        SEntityScriptComponent& operator=(const SEntityScriptComponent& Other);

        TVector<TObjectPtr<CEntityScript>> Scripts;
        
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

        /** Runs OnFixedUpdate on every ready script. Driven at the physics rate. */
        RUNTIME_API void TickFixed(ECS::FRegistry& Registry, float FixedDeltaTime);

        /** Runs OnDetach and drops every script on Entity. */
        RUNTIME_API void DetachAll(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Walks a snapshot, so an OnDetach that adds or removes scripts cannot invalidate the pool underneath.
        RUNTIME_API void DetachAllInRegistry(ECS::FRegistry& Registry);

        /**
         * One entity's scripts, serialized
         */
        struct FEvacuatedScripts
        {
            // The CWorld or CPrefab owning the registry Entity lives in. A prefab asset holds script objects
            // of its own, and one of those blocks a layout rebuild exactly as a world's does.
            TWeakObjectPtr<CObject> Owner;
            ECS::FEntity            Entity = ECS::NullEntity;
            // CPrefab only: the variant delta registry rather than the resolved one.
            bool                    bVariantDelta = false;
            TVector<uint8>          Bytes;
        };

        /**
         * Serializes and detaches every script whose class is in Classes, across every live world AND every
         * loaded prefab asset.
         *
         * Returns the number of entities evacuated. OnDetach is deliberately NOT run: the scripts are coming
         * straight back, and a detach/attach pair would fire lifecycle callbacks for what the author sees as
         * an edit. They come back un-readied, so OnReady runs again on the next tick, which is the same thing
         * a scene load does.
         */
        RUNTIME_API int32 Evacuate(const THashSet<CClass*>& Classes, TVector<FEvacuatedScripts>& Out);

        /** Rebuilds the scripts Evacuate took out. Entities whose owner or entity died in between are
         *  skipped. Returns the number of entities restored. */
        RUNTIME_API int32 Restore(const TVector<FEvacuatedScripts>& Saved);

        //~ Lookup/mutation by class, backing the script-facing GetScript/AddScript/RemoveScript API. Class
        //~ rather than C# type: a C++ script is found by exactly the same call.

        /** The first script on Entity whose class IS-A ScriptClass, or null. */
        RUNTIME_API CEntityScript* Find(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass);

        /** Appends every script on Entity whose class IS-A ScriptClass. */
        RUNTIME_API void FindAll(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out);

        /** Runs OnDetach on Script and removes it from its entity. Returns false if it was not attached. */
        RUNTIME_API bool Remove(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script);

        /** Which physics callback a DispatchCollision call delivers. */
        enum class ECollisionCallback : uint8
        {
            ContactBegin,
            ContactEnd,
            OverlapBegin,
            OverlapEnd,
        };

        /** Delivers a collision event to every script on Entity. No-op when the entity has none, so the
         *  physics drain pays one lookup rather than knowing anything about scripts. */
        RUNTIME_API void DispatchCollision(ECS::FRegistry& Registry, ECS::FEntity Entity,
            ECollisionCallback Callback, const SCollisionEvent& Event);

        /** Delivers one input event to every script on Entity. */
        RUNTIME_API void DispatchInput(ECS::FRegistry& Registry, ECS::FEntity Entity, const SInputEvent& Event);

        // Hands every script on Entity this frame's action states so C# InputAction / InputAxis bindings raise their events. Native scripts have no bindings and cost one null check.
        RUNTIME_API void PollInputBindings(ECS::FRegistry& Registry, ECS::FEntity Entity, const FInputActionState* States, int32 Count, uint32 Serial, float DeltaTime);

        /** Delivers a perception event to every script on the PERCEIVER entity. bSensed picks
         *  OnTargetPerceived vs OnTargetLost. */
        RUNTIME_API void DispatchPerception(ECS::FRegistry& Registry, ECS::FEntity Perceiver,
            bool bSensed, const SPerceptionEvent& Event);
    }
}
