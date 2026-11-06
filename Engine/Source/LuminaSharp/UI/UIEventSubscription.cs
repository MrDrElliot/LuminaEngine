using System;

namespace LuminaSharp;

// Dispose to unsubscribe; leaving it undisposed is safe since the native listener releases the callback with its element, world or script generation.
public sealed class UIEventSubscription : IDisposable
{
    // Returned when the element was invalid or the connect failed.
    internal static readonly UIEventSubscription Empty = new();

    private readonly ulong World;
    private Lumina.SUIEventListener Listener;
    private DelegateBinding Binding;

    private UIEventSubscription()
    {
    }

    internal UIEventSubscription(ulong World, Lumina.SUIEventListener Listener, DelegateBinding Binding)
    {
        this.World = World;
        this.Listener = Listener;
        this.Binding = Binding;
    }

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

        UIWorld.FromHandle(World).UI.RemoveEventListener(Listener);
        Listener = default;
    }
}
