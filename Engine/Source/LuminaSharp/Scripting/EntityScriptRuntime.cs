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

    public EntityScriptRuntime(TypeLibrary Library)
    {
        this.Library = Library;
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

        Script.Entity = new Entity(Entity);
        Script.World = new Lumina.CWorld(new IntPtr(unchecked((long)World)));
        Script.Description = Description;

        try
        {
            using (Game.Push(Script.World, Script.Entity, Script))
            {
                Script.OnAttach();
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"EntityScript.OnAttach threw: {Exception}");
        }

        GCHandle Handle = GCHandle.Alloc(Script);
        LiveHandles.Add(Handle);
        return GCHandle.ToIntPtr(Handle);
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
        }

        LiveHandles.Remove(Handle);
        if (Handle.IsAllocated)
        {
            Handle.Free();
        }
    }

    /// <summary>Delivers one discrete input event to a script's OnInput (event-driven input listening).</summary>
    public void DispatchInput(IntPtr Handle, in Lumina.InputEvent Event)
    {
        if (Resolve(Handle) is not EntityScript Script)
        {
            return;
        }

        try
        {
            using var Scope = Game.Push(Script.World, Script.Entity, Script);
            Script.OnInput(Event);
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"EntityScript.OnInput threw: {Exception}");
        }
    }

    public int CallbackFlags(IntPtr Handle)
    {
        if (Resolve(Handle) is not EntityScript Script)
        {
            return 0;
        }
        return Script.Description.CallbackFlags;
    }

    public unsafe void ApplyProperties(IntPtr Handle, byte* Blob, int Length)
    {
        if (Resolve(Handle) is not EntityScript Script)
        {
            return;
        }
        Serializer.ApplyValues(Script, Script.Description.Properties, Blob, Length);
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

        foreach (GCHandle Handle in LiveHandles)
        {
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
            if (Handle.IsAllocated)
            {
                Handle.Free();
            }
        }
        LiveHandles.Clear();
    }

    private static EntityScript? Resolve(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }
        return GCHandle.FromIntPtr(Pointer).Target as EntityScript;
    }
}
