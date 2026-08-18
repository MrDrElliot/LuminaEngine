#pragma once

#include "Core/Math/Vector/VectorTypes.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/InputAction.h"

namespace Lumina
{
    class CWorld;
    class FInputContext;
}

// The gameplay-facing input surface. Prefer these over SInputComponent's query methods: a handle resolves
// its action once instead of hashing a name per call, and the receiving-context gate lives in one place
// rather than being re-derived by every caller.
namespace Lumina::Input
{
    // The context a world's gameplay may read this frame, or null when it is not receiving input: no
    // viewport, not the active one, or the editor is holding input focus. The single definition of that
    // gate, so the action queries and the raw key snapshot cannot drift apart again.
    RUNTIME_API const FInputContext* GetReceivingContext(const CWorld* World);

    // Zeroed state when the world is not receiving input or the handle names no authored action, so a
    // caller never has to null-check before reading.
    RUNTIME_API const FInputActionState& GetActionState(const CWorld* World, const FInputActionHandle& Action);

    RUNTIME_API bool  IsActionDown    (const CWorld* World, const FInputActionHandle& Action);
    RUNTIME_API bool  IsActionPressed (const CWorld* World, const FInputActionHandle& Action);
    RUNTIME_API bool  IsActionReleased(const CWorld* World, const FInputActionHandle& Action);
    RUNTIME_API bool  IsActionHeld    (const CWorld* World, const FInputActionHandle& Action);
    RUNTIME_API bool  WasActionTapped (const CWorld* World, const FInputActionHandle& Action);

    RUNTIME_API float GetActionAxis   (const CWorld* World, const FInputActionHandle& Action);
    RUNTIME_API FVector2 GetActionAxis2D(const CWorld* World, const FInputActionHandle& Action);
    RUNTIME_API float GetActionHeldTime(const CWorld* World, const FInputActionHandle& Action);

    // +1 while Positive is down, -1 while Negative is down, 0 when neither or both are. Holding both
    // cancels rather than latching whichever side is tested first.
    RUNTIME_API float GetAxisPair(const CWorld* World, const FInputActionHandle& Positive, const FInputActionHandle& Negative);

    //~ Mapping layers. Push one to change what input means right now: a Menu layer that blocks lower makes
    //~ only its own actions fire, which is what a pause screen wants. Layers are authored on CInputSettings
    //~ and pushed by name. The stack lives on the world's input context, so it is per world, not global.

    RUNTIME_API void PushLayer(const CWorld* World, FName Layer);
    RUNTIME_API bool PopLayer(const CWorld* World, FName Layer);
    RUNTIME_API bool HasLayer(const CWorld* World, FName Layer);
    RUNTIME_API void ClearLayers(const CWorld* World);

    //~ Raw device state. Prefer an authored action: these read the shared per-viewport device directly, so
    //~ they are not rebindable and every entity in the world sees the same values. The old per-entity
    //~ snapshot pretended otherwise by copying them onto each SInputComponent.

    RUNTIME_API bool IsKeyDown    (const CWorld* World, EKey Key);
    RUNTIME_API bool IsKeyPressed (const CWorld* World, EKey Key);
    RUNTIME_API bool IsKeyReleased(const CWorld* World, EKey Key);

    RUNTIME_API bool IsMouseButtonDown    (const CWorld* World, EMouseKey Button);
    RUNTIME_API bool IsMouseButtonPressed (const CWorld* World, EMouseKey Button);
    RUNTIME_API bool IsMouseButtonReleased(const CWorld* World, EMouseKey Button);

    RUNTIME_API FVector2 GetMousePosition(const CWorld* World);
    RUNTIME_API FVector2 GetMouseDelta(const CWorld* World);
    RUNTIME_API float    GetMouseWheel(const CWorld* World);
}
