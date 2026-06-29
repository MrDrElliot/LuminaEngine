using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Loads engine assets from script, the C# analog of C++ StaticLoadObject / FSoftObjectPath. Paths are
/// virtual asset paths (e.g. "/Game/Materials/Brick"). A returned object is a
/// thin wrapper over the native CObject; assign it to a component property
/// (<c>mesh.StaticMesh = Asset.Load&lt;CStaticMesh&gt;(path)</c>) to keep it alive through the engine's refcount.
/// </summary>
public static class Asset
{
    // GCHandles of in-flight async-load callbacks, so a script ALC unload can neutralize any still pending:
    // the trampoline captures user types, so a live strong handle would pin the collectible generation and,
    // on completion, resume into a dead one. Keyed by the GCHandle's IntPtr. Game-thread only.
    private static readonly HashSet<IntPtr> Pending = new();

    /// <summary>Synchronously (blocking) loads the asset at <paramref name="Path"/> as T, or null on failure.</summary>
    public static T? Load<T>(string Path) where T : NativeObject
    {
        IntPtr Pointer = Native.LoadObject(Path);
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }
        return Wrapper<T>.Create(Pointer);
    }

    /// <summary>True if an asset exists at <paramref name="Path"/> in the registry (a probe, no load).</summary>
    public static bool Exists(string Path)
    {
        return Native.AssetExists(Path);
    }

    /// <summary>
    /// Asynchronously loads the asset at <paramref name="Path"/>; <paramref name="Callback"/> runs on the
    /// game thread with the loaded T (or null), exactly once.
    /// </summary>
    public static void LoadAsync<T>(string Path, Action<T?> Callback) where T : NativeObject
    {
        // Capture T in a trampoline so the requested type survives the type-erased native round-trip: the
        // native side hands back a GCHandle to this Action, which Host.InvokeAssetCallback resolves + frees.
        Action<IntPtr> Trampoline = Pointer =>
        {
            Callback(Pointer == IntPtr.Zero ? null : Wrapper<T>.Create(Pointer));
        };
        GCHandle Handle = GCHandle.Alloc(Trampoline);
        IntPtr Token = GCHandle.ToIntPtr(Handle);
        Pending.Add(Token);
        Native.LoadObjectAsync(Path, Token);
    }

    /// <summary>Drops a completed callback from the pending set. Called by Host.InvokeAssetCallback.</summary>
    internal static void Complete(IntPtr Token)
    {
        Pending.Remove(Token);
    }

    /// <summary>Neutralizes every in-flight async-load callback before a script ALC unload: clears each
    /// trampoline's target (releasing the captured user types so they can't pin the collectible generation)
    /// while leaving the GCHandle allocated, so the eventual native completion still frees it via
    /// Host.InvokeAssetCallback and simply finds a null target (a no-op). Game-thread only.</summary>
    internal static void PurgePending()
    {
        foreach (IntPtr Token in Pending)
        {
            GCHandle Handle = GCHandle.FromIntPtr(Token);
            if (Handle.IsAllocated)
            {
                Handle.Target = null;
            }
        }
        Pending.Clear();
    }
}
