using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// The live entity scripts of a generation, so a destroy or an unload can cancel what each is still awaiting.
internal sealed class EntityScriptRuntime
{
    private readonly TypeLibrary Library;
    private readonly HashSet<GCHandle> LiveHandles = new();

    public EntityScriptRuntime(TypeLibrary Library)
    {
        this.Library = Library;
    }

    // Registers an instance created by ScriptableRuntime, so FreeAll can detach it. Description is set first, since OnAction and the profiler labels read it.
    internal void Adopt(Lumina.CEntityScript Script, GCHandle Handle)
    {
        Script.Description = Library.Describe(Script.GetType());
        lock (LiveHandles)
        {
            LiveHandles.Add(Handle);
        }

        // The driver sets the owner before the dispatch that creates the instance, so a null entity is only a defensive case.
        if (Script.Entity.IsNull)
        {
            return;
        }

        try
        {
            Script.Description.EnsureInputComponent(Script);
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"CEntityScript component injection threw: {Exception}");
        }
    }

    // Called before the free, since a kept value gets recycled into an unrelated object's handle.
    internal void Forget(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return;
        }

        GCHandle Handle = GCHandle.FromIntPtr(Pointer);
        lock (LiveHandles)
        {
            LiveHandles.Remove(Handle);
        }

        // Every destruction path releases the handle here, so this is where the script's awaits learn it is gone.
        if (Handle.Target is Lumina.CEntityScript Script)
        {
            Script.ReleaseLifetime();
        }
    }

    // Detaches every live script and drops the index, ahead of the collectible ALC unload.
    public void FreeAll()
    {
        // Snapshot, because cancelling a token can run a continuation that mutates LiveHandles.
        List<GCHandle> Snapshot;
        lock (LiveHandles)
        {
            Snapshot = new List<GCHandle>(LiveHandles);
        }
        foreach (GCHandle Handle in Snapshot)
        {
            // Already dropped by an earlier entry's continuation; don't touch it twice.
            lock (LiveHandles)
            {
                if (!LiveHandles.Remove(Handle))
                {
                    continue;
                }
            }

            // No OnDetach, since only the load context is being replaced; the native table still owns and frees the handle.
            if (Handle.Target is Lumina.CEntityScript Script)
            {
                Script.ReleaseLifetime();
            }
        }
        lock (LiveHandles)
        {
            LiveHandles.Clear();
        }
    }
}
