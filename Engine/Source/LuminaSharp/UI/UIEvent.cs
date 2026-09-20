using System;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>Blittable event payload from native; mirrors <c>Lumina::RmlUi::FUIEventData</c> byte-for-byte.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct UIEventData
{
    public int Id;
    public int Phase;
    public Lumina.FUIElement CurrentElement;
    public Lumina.FUIElement TargetElement;
    public float MouseX;
    public float MouseY;
    public int MouseButton;
    public int KeyIdentifier;
    public int Modifiers;
    public int Pad;
}

/// <summary>The kind of UI event. Values mirror RmlUi's <c>EventId</c> (see ID.h).</summary>
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

/// <summary>
/// A UI event delivered to an <see cref="UIElement.On"/> / <see cref="UIElement.OnClick(System.Action{UIEvent})"/>
/// handler. A snapshot read-only view; capture what you need (it is not valid past the handler).
/// </summary>
public readonly struct UIEvent
{
    private readonly ulong World;
    private readonly UIEventData Data;

    internal UIEvent(ulong World, UIEventData Data)
    {
        this.World = World;
        this.Data = Data;
    }

    /// <summary>The kind of event (also implied by the type you subscribed to).</summary>
    public UIEventType Type => (UIEventType)Data.Id;

    /// <summary>The deepest element the event originated on.</summary>
    public UIElement Target => new(World, Data.TargetElement);

    /// <summary>The element the listener is attached to (where bubbling currently is).</summary>
    public UIElement Current => new(World, Data.CurrentElement);

    /// <summary>Mouse button for mouse events: 0 = left, 1 = right, 2 = middle; -1 if not a mouse event.</summary>
    public int MouseButton => Data.MouseButton;

    /// <summary>Cursor X in the document's layout space (0 if not a mouse event).</summary>
    public float MouseX => Data.MouseX;

    /// <summary>Cursor Y in the document's layout space (0 if not a mouse event).</summary>
    public float MouseY => Data.MouseY;

    /// <summary>RmlUi key identifier for key events (0 if not a key event).</summary>
    public int KeyIdentifier => Data.KeyIdentifier;

    public bool Ctrl  => (Data.Modifiers & 0x1) != 0;
    public bool Shift => (Data.Modifiers & 0x2) != 0;
    public bool Alt   => (Data.Modifiers & 0x4) != 0;
    public bool Meta  => (Data.Modifiers & 0x8) != 0;
}
