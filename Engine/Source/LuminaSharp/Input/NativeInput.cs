using System;

namespace LuminaSharp;

// Native bindings for the World.Input facade. Forward to the flat LuminaSharp_Input_* exports in the
// Runtime module (DotNetGameplay.cpp), which delegate to Input:: (Input/InputQuery.h). The world is its
// CWorld* as a ulong. Game thread only.
public static unsafe partial class Native
{
    [NativeCall] public static partial int Input_IsReceivingInput(ulong World);

    [NativeCall] public static partial void Input_PushLayer(ulong World, string Layer);
    [NativeCall] public static partial int  Input_PopLayer(ulong World, string Layer);
    [NativeCall] public static partial int  Input_HasLayer(ulong World, string Layer);
    [NativeCall] public static partial void Input_ClearLayers(ulong World);

    [NativeCall] public static partial Lumina.FInputActionState Input_GetActionState(ulong World, string Action);

    [NativeCall] public static partial int Input_IsKeyDown(ulong World, int Key);
    [NativeCall] public static partial int Input_IsKeyPressed(ulong World, int Key);
    [NativeCall] public static partial int Input_IsKeyReleased(ulong World, int Key);

    [NativeCall] public static partial int Input_IsMouseButtonDown(ulong World, int Button);
    [NativeCall] public static partial int Input_IsMouseButtonPressed(ulong World, int Button);
    [NativeCall] public static partial int Input_IsMouseButtonReleased(ulong World, int Button);

    [NativeCall] public static partial Lumina.FVector2 Input_GetMousePosition(ulong World);
    [NativeCall] public static partial Lumina.FVector2 Input_GetMouseDelta(ulong World);
    [NativeCall] public static partial float Input_GetMouseWheel(ulong World);
}
