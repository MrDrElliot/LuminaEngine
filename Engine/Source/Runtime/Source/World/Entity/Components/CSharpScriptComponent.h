#pragma once
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "Scripting/ScriptValueStore.h"
#include "CSharpScriptComponent.generated.h"

namespace Lumina
{
    // Lifecycle state of one managed instance, driven by SCSharpScriptSystem over the component view.
    enum class ECSharpBindState : uint8
    {
        Unbound = 0,    // no instance yet (or generation changed, needs rebind)
        Attached = 1,   // instance created + OnAttach run; awaiting OnReady
        Ready = 2,      // OnReady run; ticking
    };

    // One C# EntityScript attached to an entity. ScriptClass + Values serialize; the rest is transient
    // binding state owned by SCSharpScriptSystem.
    REFLECT()
    struct RUNTIME_API SScriptInstance
    {
        GENERATED_BODY()

        SScriptInstance() = default;

        // Copy carries the serialized data but never the live managed binding.
        SScriptInstance(const SScriptInstance& Other)
            : ScriptClass(Other.ScriptClass)
            , Values(Other.Values)
        {
        }

        SScriptInstance& operator=(const SScriptInstance& Other)
        {
            if (this != &Other)
            {
                ScriptClass   = Other.ScriptClass;
                Values        = Other.Values;
                Instance      = nullptr;
                Generation    = -1;
                BindState     = ECSharpBindState::Unbound;
                CallbackFlags = 0;
            }
            return *this;
        }

        // Move transfers the binding (ownership), so it is the default memberwise move.
        SScriptInstance(SScriptInstance&&) noexcept            = default;
        SScriptInstance& operator=(SScriptInstance&&) noexcept = default;

        // Full C# type name to run, e.g. "Game.HelloScript".
        PROPERTY(Editable, Category = "Script")
        FString ScriptClass;

        // Per-instance values for the script's [Property] fields, laid out by its minted CScriptStruct.
        PROPERTY(Editable)
        SScriptValueStore Values;

        // Opaque managed-instance handle (a strong GCHandle, as void*). Owned by managed; freed on detach.
        void* Instance = nullptr;
        int32 Generation = -1;
        ECSharpBindState BindState = ECSharpBindState::Unbound;

        // Bitmask (DotNet::GetScriptCallbackFlags) of which callbacks the script overrides, so dispatch
        // skips the managed crossing for the rest. Transient; set when bound.
        int32 CallbackFlags = 0;
    };

    // Attaches one or more C# EntityScripts to an entity.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SScriptComponent
    {
        GENERATED_BODY()

        SScriptComponent() = default;

        // Duplicating a script entity must NOT share the source's managed bindings (each slot's copy
        // ctor resets its own transient state).
        SScriptComponent(const SScriptComponent& Other)
            : Scripts(Other.Scripts)
        {
        }

        SScriptComponent& operator=(const SScriptComponent& Other)
        {
            if (this != &Other)
            {
                Scripts = Other.Scripts;
            }
            return *this;
        }

        SScriptComponent(SScriptComponent&&) noexcept            = default;
        SScriptComponent& operator=(SScriptComponent&&) noexcept = default;

        // The scripts attached to this entity, in dispatch order.
        PROPERTY(Editable, Category = "Script")
        TVector<SScriptInstance> Scripts;
    };

    // Binds Component.Scripts[SlotIndex]: creates the managed instance, captures callback flags, and pushes
    // saved properties. Sets BindState to Attached (OnReady runs on the next system tick). Returns the
    // handle, or null on failure. Takes the component + index (not a slot reference) because the create
    // call runs user OnAttach, which may add scripts and reallocate the vector. Shared by the script
    // system and the runtime AddScript path.
    RUNTIME_API void* BindScriptInstance(uint64 World, uint32 Entity, SScriptComponent& Component, int32 SlotIndex, int32 Generation, bool bHotReload);
}
