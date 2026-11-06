using System;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Mirrors Lumina::RmlUi::FUIEventData byte for byte.
[StructLayout(LayoutKind.Sequential)]
internal struct UIEventData
{
    public int Id;
    public int Phase;
    public Lumina.SUIElement CurrentElement;
    public Lumina.SUIElement TargetElement;
    public float MouseX;
    public float MouseY;
    public int MouseButton;
    public int KeyIdentifier;
    public int Modifiers;
    public int Pad;
}

// Values mirror the RmlUi EventId in ID.h.
public enum UIEventType
{
    Invalid = 0,
    MouseDown, MouseScroll, MouseOver, MouseOut, Focus, Blur,
    KeyDown, KeyUp, TextInput, MouseUp, Click, DoubleClick,
    Load, Unload, Show, Hide, MouseMove,
    DragMove, Drag, DragStart, DragOver, DragDrop, DragOut, DragEnd, HandleDrag,
    Resize, Scroll, AnimationEnd, TransitionEnd,
    Change, Submit, TabChange,
}

// A read-only snapshot that is not valid past the handler.
public readonly struct UIEvent
{
    private readonly ulong World;
    private readonly UIEventData Data;

    internal UIEvent(ulong World, UIEventData Data)
    {
        this.World = World;
        this.Data = Data;
    }

    public UIEventType Type => (UIEventType)Data.Id;

    // The deepest element the event originated on.
    public ref readonly UIElement Target => ref UIElement.Hold(new(World, Data.TargetElement));

    // The element the listener is attached to, where bubbling currently is.
    public ref readonly UIElement Current => ref UIElement.Hold(new(World, Data.CurrentElement));

    // 0 left, 1 right, 2 middle, or -1 for a non-mouse event.
    public int MouseButton => Data.MouseButton;

    // Cursor position in the document's layout space, 0 for a non-mouse event.
    public float MouseX => Data.MouseX;

    public float MouseY => Data.MouseY;

    // RmlUi key identifier, 0 for a non-key event.
    public int KeyIdentifier => Data.KeyIdentifier;

    public bool Ctrl  => (Data.Modifiers & 0x1) != 0;
    public bool Shift => (Data.Modifiers & 0x2) != 0;
    public bool Alt   => (Data.Modifiers & 0x4) != 0;
    public bool Meta  => (Data.Modifiers & 0x8) != 0;
}
