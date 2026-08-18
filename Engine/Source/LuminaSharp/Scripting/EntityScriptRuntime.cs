using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Owns the live EntityScript instances for one loaded generation. Native links each via a GCHandle stored on
// SScriptComponent (dereferenced directly per call, no lookup map); LiveHandles only exists to free them
// all before the collectible ALC unloads. Lifecycle is driven entirely by the native ECS view.
internal sealed class EntityScriptRuntime
{
    private readonly TypeLibrary Library;
    private readonly HashSet<GCHandle> LiveHandles = new();

    // Live scripts indexed by (world, entity) so GetScript/GetScripts resolve without a native crossing,
    // and an entity can carry several scripts. Kept in sync with Create/Destroy/FreeAll.
    private readonly Dictionary<(ulong World, uint Entity), List<EntityScript>> ByEntity = new();

    // The runtime for the current generation; GetScript/AddScript route through it.
    public static EntityScriptRuntime? Current { get; private set; }

    public EntityScriptRuntime(TypeLibrary Library)
    {
        this.Library = Library;
        Current = this;
    }

    /// <summary>The first live script of type T on the entity, or null.</summary>
    public T? GetScript<T>(ulong World, uint Entity) where T : EntityScript
    {
        if (ByEntity.TryGetValue((World, Entity), out List<EntityScript>? Scripts))
        {
            foreach (EntityScript Script in Scripts)
            {
                if (Script is T Typed)
                {
                    return Typed;
                }
            }
        }
        return null;
    }

    /// <summary>Appends every live script of type T on the entity to Out.</summary>
    public void CollectScripts<T>(ulong World, uint Entity, List<T> Out) where T : EntityScript
    {
        if (ByEntity.TryGetValue((World, Entity), out List<EntityScript>? Scripts))
        {
            foreach (EntityScript Script in Scripts)
            {
                if (Script is T Typed)
                {
                    Out.Add(Typed);
                }
            }
        }
    }

    private void IndexAdd(ulong World, uint Entity, EntityScript Script)
    {
        (ulong, uint) Key = (World, Entity);
        if (!ByEntity.TryGetValue(Key, out List<EntityScript>? Scripts))
        {
            Scripts = new List<EntityScript>();
            ByEntity[Key] = Scripts;
        }
        Scripts.Add(Script);
    }

    /// <summary>Registers an instance created by ScriptableRuntime. LiveHandles is the liveness test every
    /// dispatch resolves through, and Destroy's free path claims membership before running OnDetach.</summary>
    internal void Adopt(GCHandle Handle, EntityScript Script)
    {
        LiveHandles.Add(Handle);
        IndexAdd((ulong)(long)Script.World.Handle, Script.Entity.Id, Script);
    }

    private void IndexRemove(EntityScript Script)
    {
        // The world comes from the native object now; a script whose native side is gone has no index entry
        // left to remove, so resolving it defensively is enough.
        (ulong, uint) Key = ((ulong)(long)Script.World.Handle, Script.Entity.Id);
        if (ByEntity.TryGetValue(Key, out List<EntityScript>? Scripts))
        {
            Scripts.Remove(Script);
            if (Scripts.Count == 0)
            {
                ByEntity.Remove(Key);
            }
        }
    }

    public IReadOnlyCollection<string> TypeNames => Library.EntityScriptTypeNames;

    /// <summary>Resolves an EntityScript type from this generation by full name (for the native dynamic
    /// invoker). Engine types resolve from LuminaSharp.dll directly; only script types come through here.</summary>
    public Type? FindType(string Name)
    {
        return Library.GetEntityScript(Name)?.Type;
    }

    /// <summary>The canonical current full name for a script reference, or null if it resolves to no live type.</summary>
    public string? ResolveName(string Name)
    {
        return Library.ResolveScriptName(Name);
    }

    /// <summary>Instantiates the named EntityScript for an entity, runs OnAttach, and returns a strong
    /// GCHandle (as IntPtr) the native component stores; IntPtr.Zero on failure.</summary>
    public IntPtr Create(string TypeName, ulong World, uint Entity)
    {
        TypeDescription? Description = Library.GetEntityScript(TypeName);
        if (Description == null)
        {
            Native.Log(ELogLevel.Warn, $"EntityScript type not found: '{TypeName}'.");
            return IntPtr.Zero;
        }

        if (Description.Create() is not EntityScript Script)
        {
            Native.Log(ELogLevel.Error, $"Failed to create EntityScript '{TypeName}'.");
            return IntPtr.Zero;
        }

        // RETIRED. An EntityScript is now a CEntityScript CObject: the native driver creates it
        // (EntityScripts::Attach -> NewObject on the minted CClass), and the managed instance follows from the
        // shim's first dispatch via Scriptable::GetOrCreateInstance. An Activator-created script would have an
        // unbound NativeObject handle, so every accessor on it would throw -- failing loudly here beats
        // handing back something that looks alive.
        Native.Log(ELogLevel.Error,
            $"EntityScriptRuntime.Create is retired ('{TypeName}'): attach scripts through the native " +
            "CEntityScript path. This call site has not been ported yet.");
        _ = Description;
        return IntPtr.Zero;
    }

    public void OnReady(IntPtr Handle)
    {
        if (Resolve(Handle) is not EntityScript Script)
        {
            return;
        }

        using (Game.Push(Script.World, Script.Entity, Script))
        {
            try
            {
                Script.Description.InjectRequiredComponents(Script);
                Script.Description.EnsureInputComponent(Script);
            }
            catch (Exception Exception)
            {
                Native.Log(ELogLevel.Error, $"EntityScript [RequireComponent] injection threw: {Exception}");
            }

            try
            {
                Script.OnReady();
            }
            catch (Exception Exception)
            {
                Native.Log(ELogLevel.Error, $"EntityScript.OnReady threw: {Exception}");
            }
        }
    }

    /// <summary>One crossing per world per frame: OnUpdate the handles native gathered (only scripts that override
    /// OnUpdate, pre-filtered by the callback flag).</summary>
    public unsafe void Update(IntPtr* Handles, int Count, float DeltaTime)
    {
        bool Profiling = Profiler.Enabled;
        for (int Index = 0; Index < Count; Index++)
        {
            if (Resolve(Handles[Index]) is not EntityScript Script)
            {
                continue;
            }

            try
            {
                if (Profiling)
                {
                    Profiler.Begin(Script.Description.ProfileLabel);
                }
                try
                {
                    using (Game.Push(Script.World, Script.Entity, Script))
                    {
                        Script.OnUpdate(DeltaTime);
                    }
                }
                finally
                {
                    if (Profiling)
                    {
                        Profiler.End();
                    }
                }
            }
            catch (Exception Exception)
            {
                Native.Log(ELogLevel.Error, $"EntityScript.OnUpdate threw: {Exception}");
            }
        }
    }

    /// <summary>Dispatches OnFixedUpdate to a batch of scripts (one crossing per fixed step per world). Called
    /// 0..N times per frame by the native fixed-update accumulator, before the physics step.</summary>
    public unsafe void FixedUpdate(IntPtr* Handles, int Count, float FixedDeltaTime)
    {
        bool Profiling = Profiler.Enabled;
        for (int Index = 0; Index < Count; Index++)
        {
            if (Resolve(Handles[Index]) is not EntityScript Script)
            {
                continue;
            }

            try
            {
                if (Profiling)
                {
                    Profiler.Begin(Script.Description.FixedProfileLabel);
                }
                try
                {
                    using (Game.Push(Script.World, Script.Entity, Script))
                    {
                        Script.OnFixedUpdate(FixedDeltaTime);
                    }
                }
                finally
                {
                    if (Profiling)
                    {
                        Profiler.End();
                    }
                }
            }
            catch (Exception Exception)
            {
                Native.Log(ELogLevel.Error, $"EntityScript.OnFixedUpdate threw: {Exception}");
            }
        }
    }

    public void Destroy(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return;
        }

        GCHandle Handle = GCHandle.FromIntPtr(Pointer);

        // Claim the handle before running any user code. Removing first makes a re-entrant Destroy (OnDetach
        // destroying this same script) a no-op instead of a double free, and keeps FreeAll's contract that a
        // handle absent from the set has already been freed. GCHandle.IsAllocated cannot serve here: it only
        // tests this struct copy's raw value, so it still reads true for a pointer someone else freed.
        if (!LiveHandles.Remove(Handle))
        {
            return;
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
                Native.Log(ELogLevel.Error, $"EntityScript.OnDetach threw: {Exception}");
            }
            Script.UnbindAllDelegates();
            Script.CancelDestroyToken();
            IndexRemove(Script);
        }

        Handle.Free();
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

        // Iterate a snapshot: a script's OnDetach may remove a sibling script (routing through Destroy,
        // which mutates LiveHandles), which would otherwise throw "collection modified" mid-iteration.
        // LiveHandles membership is the source of truth for "still alive": Destroy removes from it BEFORE
        // freeing, so a handle no longer in the set has already been freed. We must consult it rather than
        // GCHandle.IsAllocated, because GCHandle is a value type -- Destroy's Free() zeroes only its own
        // struct copy, so this snapshot's copy would still report IsAllocated and double-free.
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

            // OnDetach may have destroyed siblings (or, rarely, this script itself). Only free if still
            // present: Remove returns false when Destroy already pulled it, guarding against a double-free.
            if (LiveHandles.Remove(Handle) && Handle.IsAllocated)
            {
                Handle.Free();
            }
        }
        LiveHandles.Clear();
        ByEntity.Clear();

        // Drop the static link to this now-dead generation. Current lives in the non-collectible default
        // ALC, so if it kept pointing here it would root this runtime's TypeLibrary (and its user Type
        // objects) through the collectible ALC's unload GC loop, blocking the unload on every reload.
        if (Current == this)
        {
            Current = null;
        }
    }

    /// <summary>Resolves a handle native gathered earlier in this frame. Native dispatches in batches, so a
    /// script that destroys another entity (or removes another script) from its callback frees a handle the
    /// rest of the batch still names. LiveHandles membership is the liveness test -- GCHandle equality and
    /// hashing compare the raw value without touching the target, so an already-freed pointer is rejected
    /// here instead of being dereferenced.</summary>
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
