using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Type lookup, editor metadata, and the unload handle set; lifecycle and per-frame dispatch are native, arriving through the Reflector's Scriptable shim on CEntityScript.
internal sealed class EntityScriptRuntime
{
    private readonly TypeLibrary Library;
    private readonly HashSet<GCHandle> LiveHandles = new();

    public EntityScriptRuntime(TypeLibrary Library)
    {
        this.Library = Library;
    }

    // Registers an instance created by ScriptableRuntime, so FreeAll can detach it.
    internal void Adopt(GCHandle Handle)
    {
        LiveHandles.Add(Handle);
    }

    // Called before the free, since a kept value gets recycled into an unrelated object's handle.
    internal void Forget(IntPtr Pointer)
    {
        if (Pointer != IntPtr.Zero)
        {
            LiveHandles.Remove(GCHandle.FromIntPtr(Pointer));
        }
    }

    public IReadOnlyCollection<string> TypeNames => Library.EntityScriptTypeNames;

    public byte[]? Schema(string TypeName)
    {
        TypeDescription? Description = Library.GetEntityScript(TypeName);
        return Description != null ? Serializer.WriteSchema(Description) : null;
    }

    public byte[]? Buttons(string TypeName)
    {
        TypeDescription? Description = Library.GetEntityScript(TypeName);
        return Description != null ? Serializer.WriteButtons(Description) : null;
    }

    // Detaches every live script and drops the index, ahead of the collectible ALC unload.
    public void FreeAll()
    {
        // Snapshot, because cancelling a token can run a continuation that mutates LiveHandles.
        foreach (GCHandle Handle in new List<GCHandle>(LiveHandles))
        {
            // Already dropped by an earlier entry's continuation; don't touch it twice.
            if (!LiveHandles.Contains(Handle))
            {
                continue;
            }

            // No OnDetach: the script is not detaching, its load context is being replaced. The native
            // driver delivers OnReloaded once the next generation is live.
            if (Handle.Target is EntityScript Script)
            {
                Script.CancelDestroyToken();
            }

            // Not freed here; the native instance table owns these handles and frees them as it drains.
            LiveHandles.Remove(Handle);
        }
        LiveHandles.Clear();
    }
}
