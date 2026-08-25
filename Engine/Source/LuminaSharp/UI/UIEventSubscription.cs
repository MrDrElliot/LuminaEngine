using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// A live UI event listener, returned by <see cref="UIElement.On"/> / <see cref="UIElement.OnClick(System.Action{UIEvent})"/>.
/// Dispose it to unsubscribe and release the callback. World-scoped: dispose before the world is destroyed
/// (e.g. in <see cref="EntityScript.OnDetach"/>). A subscription whose element/world is already gone is inert;
/// leaving it undisposed leaks only the managed callback (mirrors <see cref="RegistrySubscription"/>).
/// </summary>
public sealed class UIEventSubscription : IDisposable
{
    /// <summary>An inert subscription (returned when the element was invalid or the connect failed).</summary>
    internal static readonly UIEventSubscription Empty = new();

    // An untracked subscription pins the whole generation, since Handle is strong and holds the callback.
    private static readonly HashSet<UIEventSubscription> Live = new();

    private readonly ulong World;
    private IntPtr Listener;
    private GCHandle Handle;

    private UIEventSubscription()
    {
    }

    internal UIEventSubscription(ulong World, IntPtr Listener, GCHandle Handle)
    {
        this.World = World;
        this.Listener = Listener;
        this.Handle = Handle;

        Live.Add(this);
    }

    /// <summary>True while connected; false for an inert subscription or after <see cref="Dispose"/>.</summary>
    public bool IsActive => Listener != IntPtr.Zero;

    public void Dispose()
    {
        Live.Remove(this);
        Disconnect();
    }

    // Drops every subscription this generation opened, for the hot reload teardown.
    internal static void ClearAll()
    {
        var Snapshot = new List<UIEventSubscription>(Live);
        Live.Clear();

        foreach (UIEventSubscription Subscription in Snapshot)
        {
            Subscription.Disconnect();
        }
    }

    // The native listener goes first, so the thunk can never resolve a handle this already freed.
    private void Disconnect()
    {
        if (Listener != IntPtr.Zero)
        {
            Native.UI_RemoveEventListener(World, Listener);
            Listener = IntPtr.Zero;
        }
        if (Handle.IsAllocated)
        {
            Handle.Free();
        }
    }
}
