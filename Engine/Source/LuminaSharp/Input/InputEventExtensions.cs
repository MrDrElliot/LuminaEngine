namespace LuminaSharp;

// Convenience over the generated Lumina.SInputEvent mirror, which is emitted as a plain blittable struct
// and so cannot carry methods of its own.
public static class InputEventExtensions
{
    public static bool IsShiftDown(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Shift) != 0;

    public static bool IsCtrlDown(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Ctrl) != 0;

    public static bool IsAltDown(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Alt) != 0;

    // True when the OS auto-repeated a held key rather than the user pressing it again.
    public static bool IsRepeat(this Lumina.SInputEvent Event) => (Event.Flags & Lumina.EInputEventFlags.Repeat) != 0;

    public static bool IsKeyboard(this Lumina.SInputEvent Event) => Event.Device == Lumina.EKeyDevice.Keyboard;

    public static bool IsMouse(this Lumina.SInputEvent Event) => Event.Device == Lumina.EKeyDevice.Mouse;

    public static bool IsKeyDown(this Lumina.SInputEvent Event, Lumina.EKey Key)
        => Event.Type == Lumina.EInputEventType.KeyDown && Event.Device == Lumina.EKeyDevice.Keyboard && Event.Key == Key;

    public static bool IsKeyUp(this Lumina.SInputEvent Event, Lumina.EKey Key)
        => Event.Type == Lumina.EInputEventType.KeyUp && Event.Device == Lumina.EKeyDevice.Keyboard && Event.Key == Key;

    public static bool IsMouseDown(this Lumina.SInputEvent Event, Lumina.EMouseKey Button)
        => Event.Type == Lumina.EInputEventType.MouseDown && Event.Device == Lumina.EKeyDevice.Mouse && Event.Button == Button;

    public static bool IsMouseUp(this Lumina.SInputEvent Event, Lumina.EMouseKey Button)
        => Event.Type == Lumina.EInputEventType.MouseUp && Event.Device == Lumina.EKeyDevice.Mouse && Event.Button == Button;
}
