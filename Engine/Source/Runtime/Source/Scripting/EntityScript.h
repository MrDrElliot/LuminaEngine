#pragma once

#include <entt/entt.hpp>

#include "Containers/Array.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/InputEvent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/Entity/Events/PerceptionEvent.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "EntityScript.generated.h"

namespace Lumina
{
    class CWorld;

    /**
     * Base for a script attached to a single entity -- in EITHER language.
     *
     * This is the unification the rewrite was aimed at (Docs/CSharpScriptingRewrite.md, Phase 5). A C++ script
     * subclasses this directly. A C# script subclasses it too: REFLECT(Scriptable) means the Reflector emits a
     * shim whose overrides dispatch into a C# subclass of the generated wrapper, and the host mints a real
     * CClass for that subclass. Both end up as ordinary CObjects of a CClass deriving CEntityScript, so the
     * driver below just walks the component and calls virtuals -- it contains no language-specific code at all,
     * and gains none when a second language is added.
     *
     * Lifecycle: OnAttach on creation, OnReady before the first update, OnUpdate every frame, OnFixedUpdate at
     * the physics rate, OnDetach before destruction.
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
        entt::entity GetOwningEntity() const { return OwningEntity; }

        /** The world this script's entity lives in, or null when the registry has no world (a bare registry
         *  in a test). Resolved once at attach from the registry's CWorld* context singleton. */
        FUNCTION()
        CWorld* GetWorld() const { return OwningWorld; }

        /** Set once by the driver at attach, before OnAttach runs. */
        void SetOwner(entt::entity InEntity, CWorld* InWorld)
        {
            OwningEntity = InEntity;
            OwningWorld  = InWorld;
        }

        bool IsReady() const { return bReady; }
        void MarkReady() { bReady = true; }

    private:

        entt::entity OwningEntity = entt::null;
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

        TVector<TObjectPtr<CEntityScript>> Scripts;

        /**
         * Custom serializer, because a script is a per-entity SUBOBJECT rather than an asset reference: a
         * TObjectPtr property would serialize a path and fail to round-trip. Each script writes its class name
         * followed by its own tagged properties, so the values ride the SAME stock serializer every reflected
         * type uses (that is what Phase 4's appended FPropertys bought) and no parallel value store is needed.
         *
         * Load reconstructs the objects but cannot set their owning entity -- the entity is not known inside a
         * component's Serialize. The driver closes that: EntityScripts::Tick adopts any script whose owner is
         * null, which is the one place both the entity and the registry are in hand.
         */
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
        RUNTIME_API CEntityScript* Attach(FEntityRegistry& Registry, entt::entity Entity, CClass* ScriptClass);

        /** Drains pending OnReady, then runs OnUpdate on every attached script in the registry. */
        RUNTIME_API void Tick(FEntityRegistry& Registry, float DeltaTime);

        /** Runs OnFixedUpdate on every ready script. Driven at the physics rate. */
        RUNTIME_API void TickFixed(FEntityRegistry& Registry, float FixedDeltaTime);

        /** Runs OnDetach and drops every script on Entity. */
        RUNTIME_API void DetachAll(FEntityRegistry& Registry, entt::entity Entity);

        /**
         * One entity's scripts, serialized, while its script classes are rebuilt underneath it.
         *
         * Hot reload cannot change a minted class's property layout while instances of it are alive: an
         * object's size is baked in at allocation, so the block has to be torn down with nothing attached.
         * That makes a reload a round trip -- write every affected entity's scripts out, drop them, rebuild
         * the classes, read them back.
         *
         * The carrier is SEntityScriptComponent::Serialize, unchanged and already load-bearing for scenes:
         * class name plus tagged properties per script. Being name-keyed is what makes the round trip a
         * MIGRATION rather than a copy -- a property that still exists replays, a removed one is dropped,
         * and an added one lands on its default instead of reading someone else's bytes.
         */
        struct FEvacuatedScripts
        {
            TWeakObjectPtr<CWorld> World;
            entt::entity           Entity = entt::null;
            TVector<uint8>         Bytes;
        };

        /**
         * Serializes and detaches every script whose class is in Classes, across every live world.
         *
         * Returns the number of entities evacuated. OnDetach is deliberately NOT run: the scripts are coming
         * straight back, and a detach/attach pair would fire lifecycle callbacks for what the author sees as
         * an edit. They come back un-readied, so OnReady runs again on the next tick, which is the same thing
         * a scene load does.
         */
        RUNTIME_API int32 Evacuate(const THashSet<CClass*>& Classes, TVector<FEvacuatedScripts>& Out);

        /** Rebuilds the scripts Evacuate took out. Entities whose world or entity died in between are
         *  skipped. Returns the number of entities restored. */
        RUNTIME_API int32 Restore(const TVector<FEvacuatedScripts>& Saved);

        //~ Lookup/mutation by class, backing the script-facing GetScript/AddScript/RemoveScript API. Class
        //~ rather than C# type: a C++ script is found by exactly the same call.

        /** The first script on Entity whose class IS-A ScriptClass, or null. */
        RUNTIME_API CEntityScript* Find(FEntityRegistry& Registry, entt::entity Entity, const CClass* ScriptClass);

        /** Appends every script on Entity whose class IS-A ScriptClass. */
        RUNTIME_API void FindAll(FEntityRegistry& Registry, entt::entity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out);

        /** Runs OnDetach on Script and removes it from its entity. Returns false if it was not attached. */
        RUNTIME_API bool Remove(FEntityRegistry& Registry, entt::entity Entity, CEntityScript* Script);

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
        RUNTIME_API void DispatchCollision(FEntityRegistry& Registry, entt::entity Entity,
            ECollisionCallback Callback, const SCollisionEvent& Event);

        /** Delivers one input event to every script on Entity. */
        RUNTIME_API void DispatchInput(FEntityRegistry& Registry, entt::entity Entity, const SInputEvent& Event);

        /** Delivers a perception event to every script on the PERCEIVER entity. bSensed picks
         *  OnTargetPerceived vs OnTargetLost. */
        RUNTIME_API void DispatchPerception(FEntityRegistry& Registry, entt::entity Perceiver,
            bool bSensed, const SPerceptionEvent& Event);
    }
}
