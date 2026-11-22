namespace LuminaSharp;

// Methods for the generated SInputEvent mirror, which is a plain blittable struct.
public static class InputEventExtensions
{
    public static bool IsShiftDown(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Shift) != 0;

    public static bool IsCtrlDown(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Ctrl) != 0;

    public static bool IsAltDown(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Alt) != 0;

    // True when the OS auto-repeated a held key rather than the user pressing it again.
    public static bool IsRepeat(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Repeat) != 0;

    public static bool IsKeyboard(this Lumina.SInputEvent Event) => Event.Device == Lumina.EKeyDevice.Keyboard;

    public static bool IsMouse(this Lumina.SInputEvent Event) => Event.Device == Lumina.EKeyDevice.Mouse;

    public static bool IsGamepad(this Lumina.SInputEvent Event) => Event.Device == Lumina.EKeyDevice.Gamepad;

    public static bool IsKeyDown(this Lumina.SInputEvent Event, Lumina.EKey Key)
        => Event.Type == Lumina.EInputEventType.KeyDown && Event.Device == Lumina.EKeyDevice.Keyboard && Event.Key == Key;

    public static bool IsKeyUp(this Lumina.SInputEvent Event, Lumina.EKey Key)
        => Event.Type == Lumina.EInputEventType.KeyUp && Event.Device == Lumina.EKeyDevice.Keyboard && Event.Key == Key;

    public static bool IsMouseDown(this Lumina.SInputEvent Event, Lumina.EMouseKey Button)
        => Event.Type == Lumina.EInputEventType.MouseDown && Event.Device == Lumina.EKeyDevice.Mouse && Event.Button == Button;

    public static bool IsMouseUp(this Lumina.SInputEvent Event, Lumina.EMouseKey Button)
        => Event.Type == Lumina.EInputEventType.MouseUp && Event.Device == Lumina.EKeyDevice.Mouse && Event.Button == Button;

    public static bool IsGamepadDown(this Lumina.SInputEvent Event, Lumina.EGamepadButton Button)
        => Event.Type == Lumina.EInputEventType.GamepadDown && Event.Device == Lumina.EKeyDevice.Gamepad && Event.GamepadButton == Button;

    public static bool IsGamepadUp(this Lumina.SInputEvent Event, Lumina.EGamepadButton Button)
        => Event.Type == Lumina.EInputEventType.GamepadUp && Event.Device == Lumina.EKeyDevice.Gamepad && Event.GamepadButton == Button;
}
