#pragma once

#include "World/ECS/Registry.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/InputAction.h"
#include "Input/InputEvent.h"
#include "EntityScript.generated.h"

namespace Lumina
{
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

        // Reflected so the object graph can be walked and repointed, NoSerialize because the component
        // carries its own Serialize and a second, per-property path would fight it.
        PROPERTY(NoSerialize)
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

        //~ Lookup/mutation by class, backing the script-facing GetScript/AddScript/RemoveScript API. Class
        //~ rather than C# type: a C++ script is found by exactly the same call.

        /** The first script on Entity whose class IS-A ScriptClass, or null. */
        RUNTIME_API CEntityScript* Find(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass);

        /** Appends every script on Entity whose class IS-A ScriptClass. */
        RUNTIME_API void FindAll(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out);

        /** Runs OnDetach on Script and removes it from its entity. Returns false if it was not attached. */
        RUNTIME_API bool Remove(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script);

        /** Delivers one input event to every script on Entity. */
        RUNTIME_API void DispatchInput(ECS::FRegistry& Registry, ECS::FEntity Entity, const SInputEvent& Event);

        /** Delivers OnAction for each of ChangedActionIndices to every script on Entity. */
        RUNTIME_API void DispatchActions(ECS::FRegistry& Registry, ECS::FEntity Entity, const FInputActionState* States,
            int32 Count, TSpan<const int32> ChangedActionIndices);

    }
}
