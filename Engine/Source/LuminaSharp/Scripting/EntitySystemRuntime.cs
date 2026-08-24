using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// Owns the live <see cref="EntitySystem"/> instances for one loaded generation. The link to native is a
/// <see cref="GCHandle"/> stored as the <c>Self</c> of a native FStageSlot, so there is no per-handle
/// lookup table; the only collection kept is a flat set of live handles, freed before the ALC unloads
/// (a strong GCHandle pins its target). The reflection registry lives in the <see cref="TypeLibrary"/>.
///
/// Lifecycle mirrors the native CWorld managed-system store: native gathers descriptors, creates one
/// instance per descriptor per world, ticks each via the shared shim, and destroys on world teardown
/// (or drops stale handles after a hot reload without destroying, since the ALC already freed them).
/// </summary>
internal sealed class EntitySystemRuntime
{
    private readonly TypeLibrary Library;
    private readonly HashSet<GCHandle> LiveHandles = new();

    public EntitySystemRuntime(TypeLibrary Library)
    {
        this.Library = Library;
    }

    /// <summary>Number of discovered [EntitySystem] types in this generation (for editor diagnostics).</summary>
    public int TypeCount => Library.EntitySystemTypes.Count;

    // Max component types one access list (reads or writes) can declare; systems touch a handful at most.
    private const int MaxAccessTokens = 32;

    /// <summary>Reports every discovered EntitySystem to a native sink as (full name, stage, priority,
    /// write-ops tokens, read-ops tokens). The tokens are FComponentOps* the native side reads TypeId off to
    /// build the system's FSystemAccess (empty arrays => exclusive). Called once per type.</summary>
    public unsafe void Enumerate(IntPtr Sink, IntPtr Context)
    {
        if (Sink == IntPtr.Zero)
        {
            return;
        }

        var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, int, int, IntPtr*, int, IntPtr*, int, void>)Sink;
        Span<byte> Scratch = stackalloc byte[256];
        IntPtr* Writes = stackalloc IntPtr[MaxAccessTokens];
        IntPtr* Reads = stackalloc IntPtr[MaxAccessTokens];
        foreach (Type Type in Library.EntitySystemTypes)
        {
            EntitySystemAttribute? Meta = Type.GetCustomAttribute<EntitySystemAttribute>();
            if (Meta == null || Type.FullName is not { } FullName)
            {
                continue;
            }

            // Resolve declared access to FComponentOps tokens. If any declared type fails to resolve we drop
            // ALL access for this system so it stays exclusive (under-declaring access would race silently).
            int NWrite = ResolveAccessTokens(Type.GetCustomAttributes<WritesAttribute>(), Writes, Type, "Writes");
            int NRead = ResolveAccessTokens(Type.GetCustomAttributes<ReadsAttribute>(), Reads, Type, "Reads");
            if (NWrite < 0 || NRead < 0)
            {
                NWrite = 0;
                NRead = 0;
            }

            Interop.FInteropString Encoded = new(FullName, Scratch);
            try
            {
                Add(Context, Encoded.Pointer, Encoded.Length, (int)Meta.Stage, Meta.Priority, Writes, NWrite, Reads, NRead);
            }
            finally
            {
                Encoded.Free();
            }
        }
    }

    /// <summary>Resolves an access attribute's component types to their FComponentOps tokens into Out.
    /// Returns the token count, or -1 if any declared type is not a registered component (caller then falls
    /// back to exclusive).</summary>
    private static unsafe int ResolveAccessTokens(IEnumerable<ComponentAccessAttribute> Attributes, IntPtr* Out, Type System, string Kind)
    {
        int Count = 0;
        foreach (ComponentAccessAttribute Attribute in Attributes)
        {
            foreach (Type Component in Attribute.Components)
            {
                if (Count >= MaxAccessTokens)
                {
                    Native.Log(ELogLevel.Warn, $"EntitySystem '{System.Name}' declares more than {MaxAccessTokens} {Kind} components; running exclusive.");
                    return -1;
                }

                IntPtr Token = Native.FindComponentOps(Component.Name);
                if (Token == IntPtr.Zero)
                {
                    Native.Log(ELogLevel.Warn, $"EntitySystem '{System.Name}' declares {Kind} access to '{Component.Name}', which is not a registered component; running exclusive.");
                    return -1;
                }

                Out[Count++] = Token;
            }
        }
        return Count;
    }

    /// <summary>Instantiates the named EntitySystem for a world; returns a strong GCHandle (as IntPtr)
    /// the native FStageSlot stores as Self, or IntPtr.Zero on failure.</summary>
    public IntPtr Create(string TypeName, ulong World)
    {
        Type? Type = Library.GetEntitySystem(TypeName);
        if (Type == null)
        {
            Native.Log(ELogLevel.Warn, $"EntitySystem type not found: '{TypeName}'.");
            return IntPtr.Zero;
        }

        if (Activator.CreateInstance(Type) is not EntitySystem System)
        {
            Native.Log(ELogLevel.Error, $"Failed to create EntitySystem '{TypeName}'.");
            return IntPtr.Zero;
        }

        System.World = new Lumina.CWorld(new IntPtr(unchecked((long)World)));

        GCHandle Handle = GCHandle.Alloc(System);
        LiveHandles.Add(Handle);
        return GCHandle.ToIntPtr(Handle);
    }

    /// Runs OnStartup on one instance, once, where native starts its own systems, so both see the same world state.
    public void Startup(IntPtr Handle, IntPtr Context)
    {
        if (Resolve(Handle) is not EntitySystem System)
        {
            return;
        }

        try
        {
            using (Game.PushWorld(System.World))
            {
                System.OnStartup(new SystemContext(Context));
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"EntitySystem.OnStartup threw: {Exception}");
        }
    }

    public unsafe void Tick(IntPtr Handle, IntPtr Context)
    {
        if (Resolve(Handle) is not EntitySystem System)
        {
            return;
        }

        bool Profiling = Profiler.Enabled;
        try
        {
            if (Profiling)
            {
                Profiler.Begin(System.GetType().Name);
            }
            try
            {
                using (Game.PushWorld(System.World))
                {
                    System.OnUpdate(new SystemContext(Context));
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
            Native.Log(ELogLevel.Error, $"EntitySystem.OnUpdate threw: {Exception}");
        }
    }

    public void Destroy(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return;
        }

        GCHandle Handle = GCHandle.FromIntPtr(Pointer);
        if (Handle.Target is EntitySystem System)
        {
            Teardown(System, "teardown");
        }

        LiveHandles.Remove(Handle);
        if (Handle.IsAllocated)
        {
            Handle.Free();
        }
    }

    /// <summary>Frees every live handle (running OnTeardown) so the collectible ALC can unload.</summary>
    public void FreeAll()
    {
        foreach (GCHandle Handle in LiveHandles)
        {
            if (Handle.Target is EntitySystem System)
            {
                Teardown(System, "unload");
            }
            if (Handle.IsAllocated)
            {
                Handle.Free();
            }
        }
        LiveHandles.Clear();
    }

    // Teardown runs from world teardown or a script unload, never the scheduler, so the world supplies the context.
    private static void Teardown(EntitySystem System, string Reason)
    {
        try
        {
            using (Game.PushWorld(System.World))
            {
                // A world already torn down has no context left to hand over; the callback still runs.
                System.OnTeardown(System.World.IsValid
                    ? new SystemContext(Native.WorldGetSystemContext(System.World.WorldHandle))
                    : default);
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"EntitySystem.OnTeardown threw during {Reason}: {Exception}");
        }
    }

    private static EntitySystem? Resolve(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }
        return GCHandle.FromIntPtr(Pointer).Target as EntitySystem;
    }
}
