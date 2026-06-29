using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// A live subscription to a registry signal, returned by <see cref="EntityRegistry.OnConstruct{T}"/> /
/// <see cref="EntityRegistry.OnDestroy{T}"/> / <see cref="EntityRegistry.OnUpdate{T}"/>. Dispose it to
/// unsubscribe and release the callback. Subscriptions are world-scoped: dispose them before the world is
/// destroyed (e.g. in <c>EntityScript.OnDetach</c>); a subscription whose world is already gone leaks its
/// callback rather than firing into freed memory.
/// </summary>
public sealed class RegistrySubscription : IDisposable
{
    /// <summary>An inert subscription (returned when the component type is unknown or the connect failed).</summary>
    internal static readonly RegistrySubscription Empty = new();

    // Process-wide registry of live subscriptions so a script ALC unload can force-dispose any the user
    // forgot to Dispose: the GCHandle pins a user delegate, which would otherwise pin the collectible ALC.
    // Game-thread only, so a plain set is fine.
    private static readonly HashSet<RegistrySubscription> Live = new();

    private readonly ulong WorldHandle;
    private readonly IntPtr Token;
    private readonly int Kind;
    private IntPtr Listener;
    private GCHandle Handle;

    private RegistrySubscription()
    {
    }

    internal RegistrySubscription(ulong WorldHandle, IntPtr Token, int Kind, IntPtr Listener, GCHandle Handle)
    {
        this.WorldHandle = WorldHandle;
        this.Token = Token;
        this.Kind = Kind;
        this.Listener = Listener;
        this.Handle = Handle;
        Live.Add(this);
    }

    /// <summary>Force-disposes every live subscription. Called before a script ALC unload so a subscription
    /// the user forgot to Dispose can't pin the collectible generation. The worlds outlive a reload, so the
    /// disconnect is valid. Game-thread only.</summary>
    internal static void ClearAll()
    {
        foreach (RegistrySubscription Subscription in new List<RegistrySubscription>(Live))
        {
            Subscription.Dispose();
        }
        Live.Clear();
    }

    /// <summary>True while connected; false for an inert/failed subscription or after <see cref="Dispose"/>.</summary>
    public bool IsActive => Listener != IntPtr.Zero;

    public void Dispose()
    {
        if (Listener != IntPtr.Zero)
        {
            Native.RegistryDisconnect(WorldHandle, Token, Kind, Listener);
            Listener = IntPtr.Zero;
        }
        if (Handle.IsAllocated)
        {
            Handle.Free();
        }
        Live.Remove(this);
    }
}
