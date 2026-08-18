using System;

namespace LuminaSharp;

// World.Input: poll-by-name input, for code that does not want to declare an SInputAction binding.
// Prefer a binding: it resolves its action once and raises Pressed/Released/Held instead of being asked
// every frame. Everything here returns the neutral value when the world is not receiving input.
public readonly unsafe partial struct Input
{
    private readonly ulong Handle;

    internal Input(ulong World)
    {
        Handle = World;
    }

    public bool IsValid => Handle != 0;

    // False when the world has no viewport, is not the active one, or the editor holds input focus.
    public bool IsReceivingInput => IsValid && Native.Input_IsReceivingInput(Handle) != 0;

    // Pushes a mapping layer authored in the Input settings. A blocking layer stops any action it does not
    // list from reaching gameplay, which is what a pause menu wants. Re-pushing moves it to the top.
    public void PushLayer(string Layer)
    {
        if (IsValid)
        {
            Native.Input_PushLayer(Handle, Layer);
        }
    }

    // False if the layer was not on the stack.
    public bool PopLayer(string Layer) => IsValid && Native.Input_PopLayer(Handle, Layer) != 0;

    public bool HasLayer(string Layer) => IsValid && Native.Input_HasLayer(Handle, Layer) != 0;

    public void ClearLayers()
    {
        if (IsValid)
        {
            Native.Input_ClearLayers(Handle);
        }
    }

    // This frame's full evaluated state for an authored action; zeroed if the name is not one.
    public Lumina.FInputActionState GetActionState(string Action)
        => IsValid ? Native.Input_GetActionState(Handle, Action) : default;

    public bool IsActionDown(string Action) => GetActionState(Action).IsDown;

    public bool WasActionPressed(string Action) => GetActionState(Action).IsPressed;

    public bool WasActionReleased(string Action) => GetActionState(Action).IsReleased;

    public bool IsActionHeld(string Action) => GetActionState(Action).IsHeld;

    public bool WasActionTapped(string Action) => GetActionState(Action).IsTapped;

    public float GetActionAxis(string Action) => GetActionState(Action).X;

    public Lumina.FVector2 GetActionAxis2D(string Action)
    {
        Lumina.FInputActionState State = GetActionState(Action);
        return new Lumina.FVector2(State.X, State.Y);
    }

    public float GetActionHeldTime(string Action) => GetActionState(Action).HeldTime;

    // +1 while Positive is down, -1 while Negative is down, 0 when neither or both are.
    public float GetAxis(string Positive, string Negative)
        => (IsActionDown(Positive) ? 1.0f : 0.0f) - (IsActionDown(Negative) ? 1.0f : 0.0f);

    // Raw device state. Not rebindable and shared by every entity in the world, so reach for an action first.
    public bool IsKeyDown(Lumina.EKey Key) => IsValid && Native.Input_IsKeyDown(Handle, (int)Key) != 0;

    public bool WasKeyPressed(Lumina.EKey Key) => IsValid && Native.Input_IsKeyPressed(Handle, (int)Key) != 0;

    public bool WasKeyReleased(Lumina.EKey Key) => IsValid && Native.Input_IsKeyReleased(Handle, (int)Key) != 0;

    public bool IsMouseButtonDown(Lumina.EMouseKey Button) => IsValid && Native.Input_IsMouseButtonDown(Handle, (int)Button) != 0;

    public bool WasMouseButtonPressed(Lumina.EMouseKey Button) => IsValid && Native.Input_IsMouseButtonPressed(Handle, (int)Button) != 0;

    public bool WasMouseButtonReleased(Lumina.EMouseKey Button) => IsValid && Native.Input_IsMouseButtonReleased(Handle, (int)Button) != 0;

    public Lumina.FVector2 MousePosition => IsValid ? Native.Input_GetMousePosition(Handle) : default;

    public Lumina.FVector2 MouseDelta => IsValid ? Native.Input_GetMouseDelta(Handle) : default;

    public float MouseWheel => IsValid ? Native.Input_GetMouseWheel(Handle) : 0.0f;
}
