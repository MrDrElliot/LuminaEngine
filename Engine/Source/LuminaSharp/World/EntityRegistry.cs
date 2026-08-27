using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// Caches a component type's native op-table token once per type (T's wrapper name == the C++ CStruct name); zero if unregistered.
internal static class ComponentOps<T> where T : NativeStruct
{
    public static readonly IntPtr Token = Native.FindComponentOps(typeof(T).Name);
}

/// The component store for a world, the C# mirror of ECS::FRegistry / FEntityRegistry. Returned wrappers point at the live component, so writes persist.
public readonly struct EntityRegistry
{
    internal readonly ulong WorldHandle; // CWorld* the native helpers resolve the registry from

    internal EntityRegistry(ulong WorldHandle)
    {
        this.WorldHandle = WorldHandle;
    }

    public bool IsValid => WorldHandle != 0;

    /// True while the entity still exists (mirrors registry.valid). A destroyed or recycled id reads false.
    public bool Valid(Entity Entity)
    {
        return Native.WorldIsValidEntity(WorldHandle, Entity.Id) != 0;
    }

    /// The component of type T on the entity, or null if absent (mirrors registry.try_get).
    public T? TryGet<T>(Entity Entity) where T : NativeStruct
    {
        IntPtr Pointer = Native.GetComponent(WorldHandle, Entity.Id, ComponentOps<T>.Token);
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }
        return Wrapper<T>.Create(Pointer);
    }

    /// The component of type T on the entity; throws if absent (mirrors registry.get).
    public T Get<T>(Entity Entity) where T : NativeStruct
    {
        return TryGet<T>(Entity) ?? throw new InvalidOperationException($"Entity {Entity.Id} has no {typeof(T).Name}.");
    }

    public bool Has<T>(Entity Entity) where T : NativeStruct
    {
        return Native.HasComponent(WorldHandle, Entity.Id, ComponentOps<T>.Token) != 0;
    }

    /// Get-or-emplace a default component and return the live wrapper (null for a tag); idempotent, never clobbers an existing instance.
    public T? Emplace<T>(Entity Entity) where T : NativeStruct
    {
        IntPtr Pointer = Native.EmplaceComponent(WorldHandle, Entity.Id, ComponentOps<T>.Token);
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }
        return Wrapper<T>.Create(Pointer);
    }

    public bool Remove<T>(Entity Entity) where T : NativeStruct
    {
        return Native.RemoveComponent(WorldHandle, Entity.Id, ComponentOps<T>.Token) != 0;
    }

    /// Alias of Emplace.
    public T? Add<T>(Entity Entity) where T : NativeStruct => Emplace<T>(Entity);

    /// The component of type T, adding a default one first if absent.
    public T? GetOrAdd<T>(Entity Entity) where T : NativeStruct => TryGet<T>(Entity) ?? Emplace<T>(Entity);

    /// Get-or-emplace by a pre-resolved op-table token (zero on failure); for the non-generic script paths.
    internal IntPtr EmplaceRaw(Entity Entity, IntPtr Token)
    {
        return Token == IntPtr.Zero ? IntPtr.Zero : Native.EmplaceComponent(WorldHandle, Entity.Id, Token);
    }

    // Registry signals (OnConstruct/OnDestroy/OnUpdate); Dispose the returned subscription to unsubscribe.
    // Build your own events by treating a component as a signal channel (Emplace/Remove/Patch it).

    /// Fires when a T component is added to an entity.
    public RegistrySubscription OnConstruct<T>(Action<Entity> Callback) where T : NativeStruct
        => Subscribe(ComponentOps<T>.Token, 0, Callback);

    /// Fires when a T component is removed from an entity (or its entity destroyed).
    public RegistrySubscription OnDestroy<T>(Action<Entity> Callback) where T : NativeStruct
        => Subscribe(ComponentOps<T>.Token, 1, Callback);

    /// Fires when a T component is patched/replaced (see Patch).
    public RegistrySubscription OnUpdate<T>(Action<Entity> Callback) where T : NativeStruct
        => Subscribe(ComponentOps<T>.Token, 2, Callback);

    /// Pulses on_update for an entity's T component so OnUpdate listeners run; mutate via Get first, then Patch.
    public void Patch<T>(Entity Entity) where T : NativeStruct
        => Native.RegistryPatch(WorldHandle, Entity.Id, ComponentOps<T>.Token);

    private RegistrySubscription Subscribe(IntPtr Token, int Kind, Action<Entity> Callback)
    {
        if (Token == IntPtr.Zero)
        {
            return RegistrySubscription.Empty;
        }

        IntPtr Listener = Native.RegistryConnect(WorldHandle, Token, Kind);
        if (Listener == IntPtr.Zero)
        {
            return RegistrySubscription.Empty;
        }

        // Binding through the listener's own delegate is what lets its destructor release this handle,
        // whether the subscription, the world or the script generation goes away first.
        IntPtr Signal = Native.RegistryGetSignalDelegate(Listener);
        DelegateBinding Binding;
        unsafe
        {
            Binding = DelegateBindings.Bind((void*)Signal,
                new PayloadInvoker<uint> { Handler = Id => Callback(new Entity(Id)) });
        }

        if (!Binding.IsValid)
        {
            Native.RegistryDisconnect(WorldHandle, Token, Kind, Listener);
            return RegistrySubscription.Empty;
        }

        return new RegistrySubscription(WorldHandle, Token, Kind, Listener, Binding);
    }

    // Script lookup goes through the native CEntityScript component: one store, one answer, and the SAME call
    // finds a C++ script (which no managed-side index could ever have known about). Wrapper<T>.ForObject hands
    // back the canonical managed instance for the object, so a C# script comes back as itself.

    /// The first live script of type T on the entity, or null. Re-fetch per use; do not cache across frames (a stored ref outlives a destroyed script).
    public T? GetScript<T>(Entity Entity) where T : EntityScript
    {
        IntPtr Script = Native.FindEntityScript(WorldHandle, Entity.Id, typeof(T).FullName!);
        return Script == IntPtr.Zero ? null : Wrapper<T>.ForObject(Script);
    }

    /// Every live script of type T on the entity (empty if none). A fresh list per call.
    public unsafe System.Collections.Generic.List<T> GetScripts<T>(Entity Entity) where T : EntityScript
    {
        string ClassName = typeof(T).FullName!;

        // Size first, then fill: the count is the return value either way, so an entity with more scripts than
        // the stack buffer holds is handled rather than silently truncated.
        int Count = Native.FindEntityScripts(WorldHandle, Entity.Id, ClassName, null, 0);
        var Result = new System.Collections.Generic.List<T>(Count);
        if (Count <= 0)
        {
            return Result;
        }

        IntPtr[] Scripts = new IntPtr[Count];
        fixed (IntPtr* Buffer = Scripts)
        {
            Count = Native.FindEntityScripts(WorldHandle, Entity.Id, ClassName, (void**)Buffer, Count);
        }

        for (int Index = 0; Index < Count && Index < Scripts.Length; ++Index)
        {
            if (Wrapper<T>.ForObject(Scripts[Index]) is { } Script)
            {
                Result.Add(Script);
            }
        }
        return Result;
    }

    /// Attach a new script of type T to the entity and return the live instance (null on failure).
    public T? AddScript<T>(Entity Entity) where T : EntityScript
    {
        return AddScript(Entity, typeof(T).FullName!) as T;
    }

    /// Attach a script of the named class to the entity and return the live instance (null on failure).
    public EntityScript? AddScript(Entity Entity, string ClassName)
    {
        IntPtr Script = Native.AddEntityScript(WorldHandle, Entity.Id, ClassName);
        return Script == IntPtr.Zero ? null : Wrapper<EntityScript>.ForObject(Script);
    }

    /// Remove the first script of type T from the entity. Returns true if one was removed.
    public bool RemoveScript<T>(Entity Entity) where T : EntityScript
    {
        IntPtr Script = Native.FindEntityScript(WorldHandle, Entity.Id, typeof(T).FullName!);
        if (Script == IntPtr.Zero)
        {
            return false;
        }
        Native.RemoveEntityScript(WorldHandle, Entity.Id, Script);
        return true;
    }

    /// True if the entity has a C# script assignable to T.
    public bool HasScript<T>(Entity Entity) where T : EntityScript
    {
        return GetScript<T>(Entity) != null;
    }

    // typed typed views (mirrors registry.view<...>); pass an Exclude.Of<...>() filter as the optional argument. Arity 1..4.

    public View<T1> View<T1>(Exclude Filter = default)
        where T1 : NativeStruct
    {
        return new View<T1>(WorldHandle, ComponentOps<T1>.Token, Filter);
    }

    /// Shorthand for View&lt;T1&gt;: iterate every entity that has a T component.
    public View<T1> All<T1>(Exclude Filter = default) where T1 : NativeStruct => View<T1>(Filter);

    public View<T1, T2> View<T1, T2>(Exclude Filter = default)
        where T1 : NativeStruct
        where T2 : NativeStruct
    {
        return new View<T1, T2>(WorldHandle, ComponentOps<T1>.Token, ComponentOps<T2>.Token, Filter);
    }

    public View<T1, T2, T3> View<T1, T2, T3>(Exclude Filter = default)
        where T1 : NativeStruct
        where T2 : NativeStruct
        where T3 : NativeStruct
    {
        return new View<T1, T2, T3>(WorldHandle, ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, Filter);
    }

    public View<T1, T2, T3, T4> View<T1, T2, T3, T4>(Exclude Filter = default)
        where T1 : NativeStruct
        where T2 : NativeStruct
        where T3 : NativeStruct
        where T4 : NativeStruct
    {
        return new View<T1, T2, T3, T4>(WorldHandle, ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, ComponentOps<T4>.Token, Filter);
    }
}
