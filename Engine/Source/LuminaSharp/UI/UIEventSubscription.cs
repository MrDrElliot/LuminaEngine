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
    private IntPtr Listener;
    private DelegateBinding Binding;

    private UIEventSubscription()
    {
    }

    internal UIEventSubscription(ulong World, IntPtr Listener, DelegateBinding Binding)
    {
        this.World = World;
        this.Listener = Listener;
        this.Binding = Binding;
    }

    /// <summary>True while connected; false for an inert subscription or after <see cref="Dispose"/>.</summary>
    public bool IsActive => Listener != IntPtr.Zero;

    public void Dispose()
    {
        if (Listener == IntPtr.Zero)
        {
            return;
        }

        // Unbind first, so the listener's destructor has nothing left to report to the managed registry.
        Binding.Dispose();
        Binding = default;

        Native.UI_RemoveEventListener(World, Listener);
        Listener = IntPtr.Zero;
    }
}
