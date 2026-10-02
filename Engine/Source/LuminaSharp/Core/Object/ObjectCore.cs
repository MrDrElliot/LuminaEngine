using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// The script half of ObjectCore.h, imported into every script with a global using static so calls read as they do in C++.
public static class ObjectCore
{
    // Pending async callbacks, so a script unload can clear what they capture before the native side completes them.
    private static readonly HashSet<IntPtr> PendingLoads = new();

    // Blocking load by virtual path, as LoadObject<T> in C++; the wrapper holds a strong reference like a TObjectPtr member.
    public static T? LoadObject<T>(string Path) where T : NativeObject
    {
        IntPtr Pointer = Native.LoadObject(Path);
        return Pointer == IntPtr.Zero ? null : Wrapper<T>.ForObject(Pointer);
    }

    // As AsyncLoadObject in C++, the callback runs once on the game thread with the loaded object or null.
    public static void AsyncLoadObject<T>(string Path, Action<T?> Callback) where T : NativeObject
    {
        // The trampoline keeps T across the type-erased native round trip, and Host.InvokeAssetCallback frees its handle.
        Action<IntPtr> Trampoline = Pointer => Callback(Pointer == IntPtr.Zero ? null : Wrapper<T>.ForObject(Pointer));
        IntPtr Token = GCHandle.ToIntPtr(GCHandle.Alloc(Trampoline));
        PendingLoads.Add(Token);
        Native.LoadObjectAsync(Path, Token);
    }

    internal static void CompleteAsyncLoad(IntPtr Token)
    {
        PendingLoads.Remove(Token);
    }

    // Drops what pending callbacks capture before an unload, leaving each handle for the native completion to free.
    internal static void PurgePendingLoads()
    {
        foreach (IntPtr Token in PendingLoads)
        {
            GCHandle Handle = GCHandle.FromIntPtr(Token);
            if (Handle.IsAllocated)
            {
                Handle.Target = null;
            }
        }
        PendingLoads.Clear();
    }
}
