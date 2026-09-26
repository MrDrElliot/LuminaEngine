using System;

namespace LuminaSharp;

/// <summary>
/// A live UI event listener, returned by <see cref="UIElement.On"/> / <see cref="UIElement.OnClick(System.Action{UIEvent})"/>.
/// Dispose it to unsubscribe. Undisposed is safe: the native listener owns the binding, so destroying the
/// element, the world or the script generation releases the callback on its own.
/// </summary>
public sealed class UIEventSubscription : IDisposable
{
    /// <summary>An inert subscription (returned when the element was invalid or the connect failed).</summary>
    internal static readonly UIEventSubscription Empty = new();

    private readonly ulong World;
    private Lumina.FUIEventListener Listener;
    private DelegateBinding Binding;

    private UIEventSubscription()
    {
    }

    internal UIEventSubscription(ulong World, Lumina.FUIEventListener Listener, DelegateBinding Binding)
    {
        this.World = World;
        this.Listener = Listener;
        this.Binding = Binding;
    }

    /// <summary>True while connected; false for an inert subscription or after <see cref="Dispose"/>.</summary>
    public bool IsActive => Listener.IsValid;

    public void Dispose()
    {
        if (!Listener.IsValid)
        {
            return;
        }

        // Unbind first: the listener owns the delegate, so after it goes the binding has nothing to name.
        Binding.Dispose();
        Binding = default;

        Lumina.CUILibrary.RemoveEventListener(UI.WorldOf(World), Listener);
        Listener = default;
    }
}
