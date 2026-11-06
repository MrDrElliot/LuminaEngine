using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Copyable handle to an element in a loaded UIDocument; calls on an invalid element are safe no-ops.
public readonly unsafe struct UIElement
{
    internal readonly ulong World;
    internal readonly Lumina.SUIElement Handle;

    internal UIElement(ulong World, Lumina.SUIElement Handle)
    {
        this.World = World;
        this.Handle = Handle;
    }

    // Indexers and properties hand elements out by readonly ref, which is what lets doc["id"].Text = ... compile.
    internal static ref readonly UIElement Hold(UIElement Value) => ref HeldElement<UIElement>.Hold(Value);

    // False if a query missed or the owning document was closed.
    public bool IsValid => Handle.IsValid;

    // The setter escapes its value, while the getter returns inner RML markup.
    public string Text
    {
        get => Rml;
        set { if (IsValid) Lumina.CUILibrary.SetInnerRml(Handle, Escape(value)); }
    }

    // Set is not escaped, so pass real markup.
    public string Rml
    {
        get => IsValid ? Lumina.CUILibrary.GetInnerRml(Handle) : string.Empty;
        set { if (IsValid) Lumina.CUILibrary.SetInnerRml(Handle, value); }
    }

    // Escaped, same as the Text setter.
    public void SetText(string Value)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.SetInnerRml(Handle, Escape(Value));
        }
    }

    public string GetAttribute(string Name) => IsValid ? Lumina.CUILibrary.GetAttribute(Handle, Name) : string.Empty;

    public void SetAttribute(string Name, string Value)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.SetAttribute(Handle, Name, Value);
        }
    }

    public void SetStyle(string Property, string Value)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.SetProperty(Handle, Property, Value);
        }
    }

    // A length in density independent pixels, the unit RML layouts are written in.
    public void SetDp(string Property, float Value) => SetStyle(Property, Value.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "dp");

    // A length as a percentage of the parent, where 50 is half.
    public void SetPercent(string Property, float Value) => SetStyle(Property, Value.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "%");

    // Zero is invisible but still laid out, unlike hiding the element.
    public void SetOpacity(float Value) => SetStyle("opacity", Math.Clamp(Value, 0.0f, 1.0f).ToString("0.###", System.Globalization.CultureInfo.InvariantCulture));

    // Reverts to the stylesheet value.
    public void ClearStyle(string Property)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.RemoveProperty(Handle, Property);
        }
    }

    public void AddClass(string Class)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.SetClass(Handle, Class, true);
        }
    }

    public void RemoveClass(string Class)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.SetClass(Handle, Class, false);
        }
    }

    public void ToggleClass(string Class, bool Active)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.SetClass(Handle, Class, Active);
        }
    }

    public bool HasClass(string Class) => IsValid && Lumina.CUILibrary.IsClassSet(Handle, Class);

    // Chained calls off an indexer need SetVisible, since a struct property cannot be assigned on a non-variable.
    public bool Visible
    {
        set { if (IsValid) Lumina.CUILibrary.SetProperty(Handle, "display", value ? "block" : "none"); }
    }

    public void SetVisible(bool Value)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.SetProperty(Handle, "display", Value ? "block" : "none");
        }
    }

    public void Focus()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.FocusElement(Handle);
        }
    }

    public void Blur()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.BlurElement(Handle);
        }
    }

    // Fires click listeners and follows the active state.
    public void Click()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.ClickElement(Handle);
        }
    }

    // Border box in document pixels, all zero for an invalid element.
    public (float X, float Y, float Width, float Height) Box
    {
        get
        {
            if (!IsValid)
            {
                return (0, 0, 0, 0);
            }
            Lumina.FVector4 XYWH = Lumina.CUILibrary.GetElementBox(Handle);
            return (XYWH.X, XYWH.Y, XYWH.Z, XYWH.W);
        }
    }

    // First descendant matching a CSS selector, or an invalid element.
    public UIElement Query(string Selector) => new(World, IsValid ? Lumina.CUILibrary.QuerySelector(Handle, Selector) : default);

    // ---- events ----

    // The handler runs on the game thread; dispose the subscription before the world tears down, e.g. in OnDetach.
    public UIEventSubscription On(string EventType, Action<UIEvent> Handler)
    {
        if (!IsValid || string.IsNullOrEmpty(EventType))
        {
            return UIEventSubscription.Empty;
        }

        // A struct can't be captured by a lambda, so copy the world handle into a local first.
        ulong WorldId = World;

        // RmlUi dispatches outside every script callback, so the handler re-enters the subscriber's context.
        Lumina.CEntityScript? Owner = Engine.ActiveScript;

        Lumina.SUIEventListener Listener = UIWorld.FromHandle(WorldId).UI.AddEventListener(Handle, EventType);
        if (!Listener.IsValid)
        {
            return UIEventSubscription.Empty;
        }

        // Bound through the listener's own delegate, so its destructor releases this handle whichever owner dies first.
        ulong Event = Lumina.CUILibrary.GetEventListenerDelegate(Listener);
        DelegateBinding Binding = DelegateBindings.Bind((void*)Event,
            new PayloadInvoker<UIEventData>
            {
                Handler = Data =>
                {
                    using Engine.Scope Scope = Owner != null && !Owner.Entity.IsNull
                        ? Engine.Push(Owner.World, Owner.Entity, Owner)
                        : Engine.Push(UIWorld.FromHandle(WorldId));
                    Handler(new UIEvent(WorldId, Data));
                },
            });

        if (!Binding.IsValid)
        {
            UIWorld.FromHandle(WorldId).UI.RemoveEventListener(Listener);
            return UIEventSubscription.Empty;
        }

        return new UIEventSubscription(WorldId, Listener, Binding);
    }

    public UIEventSubscription OnClick(Action<UIEvent> Handler) => On("click", Handler);

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
