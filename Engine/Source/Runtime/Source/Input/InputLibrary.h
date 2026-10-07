#pragma once

#include "Containers/Name.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/InputAction.h"
#include "Input/InputMode.h"
#include "InputLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    /** Polling input for a world. Everything returns the neutral value when the world is not receiving input. */
    REFLECT()
    class RUNTIME_API CInputLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** False when the world has no viewport, is not the active one, or the editor holds input focus. */
        FUNCTION()
        static bool IsReceivingInput(CWorld* World);

        //~ Authored actions, the rebindable surface.

        /** This frame's evaluated state, zeroed when the name is not an authored action. */
        FUNCTION()
        static FInputActionState GetActionState(CWorld* World, const FName& Action);

        FUNCTION()
        static bool IsActionDown(CWorld* World, const FName& Action);

        FUNCTION()
        static bool WasActionPressed(CWorld* World, const FName& Action);

        FUNCTION()
        static bool WasActionReleased(CWorld* World, const FName& Action);

        FUNCTION()
        static bool IsActionHeld(CWorld* World, const FName& Action);

        FUNCTION()
        static bool WasActionTapped(CWorld* World, const FName& Action);

        FUNCTION()
        static float GetActionAxis(CWorld* World, const FName& Action);

        FUNCTION()
        static FVector2 GetActionAxis2D(CWorld* World, const FName& Action);

        FUNCTION()
        static float GetActionHeldTime(CWorld* World, const FName& Action);

        /** Plus one while Positive is down and minus one while Negative is, so holding both cancels. */
        FUNCTION()
        static float GetAxisPair(CWorld* World, const FName& Positive, const FName& Negative);

        /** Row of the action in the per-frame state table, or Constants::kIndexNone. Resolve once per settings serial. */
        FUNCTION()
        static int32 FindActionIndex(const FName& Action);

        //~ Mapping layers, per world. A blocking layer stops any action it does not list.

        FUNCTION()
        static void PushLayer(CWorld* World, const FName& Layer);

        /** False when the layer was not on the stack. */
        FUNCTION()
        static bool PopLayer(CWorld* World, const FName& Layer);

        FUNCTION()
        static bool HasLayer(CWorld* World, const FName& Layer);

        FUNCTION()
        static void ClearLayers(CWorld* World);

        //~ Raw device state, shared by every entity in the world and not rebindable.

        FUNCTION()
        static bool IsKeyDown(CWorld* World, EKey Key);

        FUNCTION()
        static bool WasKeyPressed(CWorld* World, EKey Key);

        FUNCTION()
        static bool WasKeyReleased(CWorld* World, EKey Key);

        FUNCTION()
        static bool IsMouseButtonDown(CWorld* World, EMouseKey Button);

        FUNCTION()
        static bool WasMouseButtonPressed(CWorld* World, EMouseKey Button);

        FUNCTION()
        static bool WasMouseButtonReleased(CWorld* World, EMouseKey Button);

        FUNCTION()
        static FVector2 GetMousePosition(CWorld* World);

        FUNCTION()
        static FVector2 GetMouseDelta(CWorld* World);

        FUNCTION()
        static float GetMouseWheel(CWorld* World);

        //~ Cursor and routing, which act on the world's own viewport rather than the receiving one.

        FUNCTION()
        static void SetInputMode(CWorld* World, EInputMode Mode);

        FUNCTION()
        static EInputMode GetInputMode(CWorld* World);

        FUNCTION()
        static void SetMouseMode(CWorld* World, EMouseMode Mode);

        FUNCTION()
        static EMouseMode GetMouseMode(CWorld* World);
    };
}
