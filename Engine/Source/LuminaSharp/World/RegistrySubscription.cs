using System;

namespace LuminaSharp;

/// <summary>
/// A live subscription to a registry signal, returned by <see cref="EntityRegistry.OnConstruct{T}"/> /
/// <see cref="EntityRegistry.OnDestroy{T}"/> / <see cref="EntityRegistry.OnUpdate{T}"/>. Dispose it to
/// unsubscribe. Undisposed is safe: the native listener owns the binding, so tearing the world down or
/// reloading scripts releases the callback on its own.
/// </summary>
public sealed class RegistrySubscription : IDisposable
{
    /// <summary>An inert subscription (returned when the component type is unknown or the connect failed).</summary>
    internal static readonly RegistrySubscription Empty = new();

    private readonly ulong WorldHandle;
    private readonly IntPtr Token;
    private readonly int Kind;
    private IntPtr Listener;
    private DelegateBinding Binding;

    private RegistrySubscription()
    {
    }

    internal RegistrySubscription(ulong WorldHandle, IntPtr Token, int Kind, IntPtr Listener, DelegateBinding Binding)
    {
        this.WorldHandle = WorldHandle;
        this.Token = Token;
        this.Kind = Kind;
        this.Listener = Listener;
        this.Binding = Binding;
    }

    /// <summary>True while connected; false for an inert/failed subscription or after <see cref="Dispose"/>.</summary>
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

        Native.RegistryDisconnect(WorldHandle, Token, Kind, Listener);
        Listener = IntPtr.Zero;
    }
}
