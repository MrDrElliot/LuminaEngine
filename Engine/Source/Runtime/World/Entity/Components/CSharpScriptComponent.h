#pragma once
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "Scripting/ScriptValueStore.h"
#include "CSharpScriptComponent.generated.h"

namespace Lumina
{
    // Lifecycle state of the managed instance, driven by SCSharpScriptSystem over its component view.
    enum class ECSharpBindState : uint8
    {
        Unbound = 0,    // no instance yet (or generation changed -> needs rebind)
        Attached = 1,   // instance created + OnAttach run; awaiting OnReady
        Ready = 2,      // OnReady run; ticking
    };

    // Attaches a C# EntityScript to an entity.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SScriptComponent
    {
        GENERATED_BODY()

        SScriptComponent() = default;

        // Duplicating a script entity must NOT share the source's managed binding.
        SScriptComponent(const SScriptComponent& Other)
            : ScriptClass(Other.ScriptClass)
            , Values(Other.Values)
        {
        }

        SScriptComponent& operator=(const SScriptComponent& Other)
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
        SScriptComponent(SScriptComponent&&) noexcept            = default;
        SScriptComponent& operator=(SScriptComponent&&) noexcept = default;

        // Full C# type name to run on this entity, e.g. "Game.HelloScript".
        PROPERTY(Editable, Category = "Script")
        FString ScriptClass;

        // Per-instance values for the script's [Property] fields, laid out by the script's minted CScriptStruct.
        PROPERTY(Editable)
        SScriptValueStore Values;

        // Opaque managed-instance handle (a strong GCHandle, as void*). Owned by managed; freed on detach.
        void* Instance = nullptr;
        int32 Generation = -1;
        ECSharpBindState BindState = ECSharpBindState::Unbound;

        // Bitmask (DotNet::GetScriptCallbackFlags) of which collision callbacks the script overrides, so
        // physics dispatch skips the managed crossing for the rest. Transient; set when bound.
        int32 CallbackFlags = 0;
    };
}
