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

    // Registers an instance created by ScriptableRuntime, so PollInput can liveness-test its handle.
    internal void Adopt(GCHandle Handle)
    {
        LiveHandles.Add(Handle);
    }

    public IReadOnlyCollection<string> TypeNames => Library.EntityScriptTypeNames;

    /// <summary>The canonical current full name for a script reference, or null if it resolves to no live type.</summary>
    public string? ResolveName(string Name)
    {
        return Library.ResolveScriptName(Name);
    }

    /// <summary>Applies this frame's action states to a script's input bindings, raising their events. One
    /// crossing per script per frame, and only for scripts that declare a binding (callback flag) whose
    /// entity is receiving input.</summary>
    public unsafe void PollInput(IntPtr Handle, Lumina.FInputActionState* States, int Count, uint Serial, float DeltaTime)
    {
        if (Resolve(Handle) is not EntityScript Script || !Script.Description.HasInputBindings)
        {
            return;
        }

        try
        {
            using var Scope = Game.Push(Script.World, Script.Entity, Script);
            Script.Description.PollInputBindings(Script, States, Count, Serial, DeltaTime);
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"EntityScript input binding threw: {Exception}");
        }
    }

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

    /// <summary>Frees every live handle (running OnDetach) so the collectible ALC can unload. Called on
    /// reload/shutdown before the old generation's context is torn down.</summary>
    public void FreeAll()
    {
        // Detach every managed delegate binding before this collectible generation unloads.
        DelegateBindings.PurgeAll();

        // Snapshot, because an OnDetach that destroys a sibling mutates LiveHandles mid-iteration.
        foreach (GCHandle Handle in new List<GCHandle>(LiveHandles))
        {
            // Already destroyed (and OnDetach'd) by an earlier sibling's OnDetach; don't repeat either.
            if (!LiveHandles.Contains(Handle))
            {
                continue;
            }

            if (Handle.Target is EntityScript Script)
            {
                try
                {
                    using (Game.Push(Script.World, Script.Entity, Script))
                    {
                        Script.OnDetach();
                    }
                }
                catch (Exception Exception)
                {
                    Native.Log(ELogLevel.Error, $"EntityScript.OnDetach threw during unload: {Exception}");
                }
                Script.CancelDestroyToken();
            }

            // Remove returns false when an earlier OnDetach already pulled it, guarding a double-free.
            if (LiveHandles.Remove(Handle) && Handle.IsAllocated)
            {
                Handle.Free();
            }
        }
        LiveHandles.Clear();
    }

    // LiveHandles membership is the liveness test: GCHandle hashes on the raw value without touching the target, so an already-freed pointer is rejected rather than dereferenced.
    private EntityScript? Resolve(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }
        GCHandle Handle = GCHandle.FromIntPtr(Pointer);
        return LiveHandles.Contains(Handle) ? Handle.Target as EntityScript : null;
    }
}
