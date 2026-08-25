using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// A handle to one element in a loaded <see cref="UIDocument"/>. Get/set text, styles, classes and
/// attributes, and subscribe to events. A lightweight value (copy freely); valid while its document is
/// loaded and the element is in the DOM. Calls on an invalid element are safe no-ops.
/// </summary>
public readonly unsafe struct UIElement
{
    internal readonly ulong World;
    internal readonly IntPtr Ptr;

    internal UIElement(ulong World, IntPtr Ptr)
    {
        this.World = World;
        this.Ptr = Ptr;
    }

    /// <summary>False if a query missed or the owning document has been closed.</summary>
    public bool IsValid => Ptr != IntPtr.Zero;

    /// <summary>Plain text content. The setter HTML-escapes its value (so arbitrary strings render
    /// literally); the getter returns the element's inner RML markup. For markup use <see cref="Rml"/>.</summary>
    public string Text
    {
        get => Rml;
        set { if (IsValid) Native.UI_SetInnerRml(Ptr, Escape(value)); }
    }

    /// <summary>The element's inner RML markup (set is NOT escaped -- pass real markup).</summary>
    public string Rml
    {
        get => IsValid ? Native.UI_GetInnerRml(Ptr) : string.Empty;
        set { if (IsValid) Native.UI_SetInnerRml(Ptr, value); }
    }

    /// <summary>Set plain text (escaped). Same as the <see cref="Text"/> setter, as a method.</summary>
    public void SetText(string Value)
    {
        if (IsValid)
        {
            Native.UI_SetInnerRml(Ptr, Escape(Value));
        }
    }

    public string GetAttribute(string Name) => IsValid ? Native.UI_GetAttribute(Ptr, Name) : string.Empty;

    public void SetAttribute(string Name, string Value)
    {
        if (IsValid)
        {
            Native.UI_SetAttribute(Ptr, Name, Value);
        }
    }

    /// <summary>Set an inline CSS property, e.g. <c>SetStyle("color", "#ff0000")</c> or
    /// <c>SetStyle("width", "50%")</c>.</summary>
    public void SetStyle(string Property, string Value)
    {
        if (IsValid)
        {
            Native.UI_SetProperty(Ptr, Property, Value);
        }
    }

    /// <summary>Remove an inline CSS property set via <see cref="SetStyle"/>, reverting to the stylesheet.</summary>
    public void ClearStyle(string Property)
    {
        if (IsValid)
        {
            Native.UI_RemoveProperty(Ptr, Property);
        }
    }

    public void AddClass(string Class)
    {
        if (IsValid)
        {
            Native.UI_SetClass(Ptr, Class, 1);
        }
    }

    public void RemoveClass(string Class)
    {
        if (IsValid)
        {
            Native.UI_SetClass(Ptr, Class, 0);
        }
    }

    public void ToggleClass(string Class, bool Active)
    {
        if (IsValid)
        {
            Native.UI_SetClass(Ptr, Class, Active ? 1 : 0);
        }
    }

    public bool HasClass(string Class) => IsValid && Native.UI_IsClassSet(Ptr, Class) != 0;

    /// <summary>Show or hide the element via the CSS <c>display</c> property. (Property form; usable on a
    /// stored element variable. For a chained call -- <c>doc["x"].SetVisible(false)</c> -- use the method.)</summary>
    public bool Visible
    {
        set { if (IsValid) Native.UI_SetProperty(Ptr, "display", value ? "block" : "none"); }
    }

    /// <summary>Show/hide via the CSS <c>display</c> property. Method form, so it chains off an indexer
    /// result (a struct property setter can't be assigned on a non-variable).</summary>
    public void SetVisible(bool Value)
    {
        if (IsValid)
        {
            Native.UI_SetProperty(Ptr, "display", Value ? "block" : "none");
        }
    }

    public void Focus()
    {
        if (IsValid)
        {
            Native.UI_ElementFocus(Ptr);
        }
    }

    public void Blur()
    {
        if (IsValid)
        {
            Native.UI_ElementBlur(Ptr);
        }
    }

    /// <summary>Synthesize a click on this element (fires "click" listeners, follows the active state).</summary>
    public void Click()
    {
        if (IsValid)
        {
            Native.UI_ElementClick(Ptr);
        }
    }

    /// <summary>First descendant matching a CSS selector, or an invalid element.</summary>
    public UIElement Query(string Selector) => new(World, IsValid ? Native.UI_QuerySelector(Ptr, Selector) : IntPtr.Zero);

    // ---- events ----

    /// <summary>
    /// Subscribe to an RmlUi event on this element (e.g. "click", "mousedown", "change", "keydown"). The
    /// handler runs on the game thread during input/UI update. Dispose the returned subscription to
    /// unsubscribe (do so before the world tears down, e.g. in <see cref="EntityScript.OnDetach"/>).
    /// </summary>
    public UIEventSubscription On(string EventType, Action<UIEvent> Handler)
    {
        if (!IsValid || string.IsNullOrEmpty(EventType))
        {
            return UIEventSubscription.Empty;
        }

        // A struct can't be captured by a lambda, so copy the world handle into a local first.
        ulong WorldId = World;

        IntPtr Listener = Native.UI_AddEventListener(WorldId, Ptr, EventType);
        if (Listener == IntPtr.Zero)
        {
            return UIEventSubscription.Empty;
        }

        // Binding through the listener's own delegate is what lets its destructor release this handle,
        // whether the element, the world or the script generation is what goes away first.
        IntPtr Event = Native.UI_GetEventListenerDelegate(Listener);
        DelegateBinding Binding = DelegateBindings.Bind((void*)Event,
            new PayloadInvoker<UIEventData> { Handler = Data => Handler(new UIEvent(WorldId, Data)) });

        if (!Binding.IsValid)
        {
            Native.UI_RemoveEventListener(WorldId, Listener);
            return UIEventSubscription.Empty;
        }

        return new UIEventSubscription(WorldId, Listener, Binding);
    }

    /// <summary>Subscribe to "click" on this element.</summary>
    public UIEventSubscription OnClick(Action<UIEvent> Handler) => On("click", Handler);

    /// <summary>Subscribe to "click" with a no-argument handler.</summary>
    public UIEventSubscription OnClick(Action Handler) => On("click", _ => Handler());

    private static readonly char[] MarkupChars = { '&', '<', '>' };

    private static string Escape(string Value)
    {
        if (string.IsNullOrEmpty(Value) || Value.IndexOfAny(MarkupChars) < 0)
        {
            return Value ?? string.Empty;
        }
        return Value.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");
    }
}
