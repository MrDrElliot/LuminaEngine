using System;
using System.Runtime.CompilerServices;
using LuminaSharp;

namespace Lumina;

public delegate void PatchFunc<T>(ref T Component) where T : struct, IComponent<T>;

public readonly unsafe partial struct SSystemContext
{
    private static readonly FName DefaultEntityName = new("Entity");

    // The handle is valid at once either way; from a parallel system it gets its components at the flush.
    public FEntity Create(FName EntityName = default) => ScriptCreate(FTransform.Identity, NameOrDefault(EntityName));

    public FEntity Create(FVector3 Location, FName EntityName = default)
        => ScriptCreate(new FTransform(Location, FQuat.Identity, FVector3.One), NameOrDefault(EntityName));

    public FEntity Create(FTransform Transform, FName EntityName = default) => ScriptCreate(Transform, NameOrDefault(EntityName));

    // Adds a default T or replaces the one there, and returns it to fill in place: the live component from an exclusive system, or a staged one from a parallel system that lands at the flush. A tag returns a null ref.
    public ref T Emplace<T>(FEntity Entity) where T : struct, IComponent<T>
    {
        IntPtr Value = ScriptEmplace(Entity, ComponentOps<T>.Token);
        return ref Value == IntPtr.Zero ? ref Unsafe.NullRef<T>() : ref Unsafe.AsRef<T>((void*)Value);
    }

    // Adds a copy of Value, a component reached in place, or replaces the one there.
    public void Emplace<T>(FEntity Entity, in T Value) where T : struct, IComponent<T>
    {
        ScriptEmplaceCopy(Entity, ComponentOps<T>.Token, (IntPtr)Unsafe.AsPointer(ref Unsafe.AsRef(in Value)));
    }

    public void Remove<T>(FEntity Entity) where T : struct, IComponent<T> => ScriptRemove(Entity, ComponentOps<T>.Token);

    // Edits Entity's T, if it has one: at once from an exclusive system, at the flush from a parallel one, so a system can touch a component it does not query.
    public void Patch<T>(FEntity Entity, PatchFunc<T> Mutator) where T : struct, IComponent<T>
    {
        ArgumentNullException.ThrowIfNull(Mutator);
        if (CanReachWorld())
        {
            PatchNow(new SExclusiveContext(Ref).World, Entity, Mutator);
            return;
        }
        Defer(Exclusive => PatchNow(Exclusive.World, Entity, Mutator));
    }

    private static void PatchNow<T>(CWorld World, FEntity Entity, PatchFunc<T> Mutator) where T : struct, IComponent<T>
    {
        if (World.HasComponent<T>(Entity))
        {
            Mutator(ref World.GetComponent<T>(Entity));
        }
    }

    public SystemPhysics Physics => new(this);

    // Runs Work at the next flush on the main thread with the world in reach, for anything a system cannot do beside others.
    public void Defer(Action<SExclusiveContext> Work)
    {
        ArgumentNullException.ThrowIfNull(Work);
        ScriptDefer(ScriptCallback.Bind(new DeferredWork(Work)));
    }

    private static FName NameOrDefault(FName EntityName) => EntityName.IsNone ? DefaultEntityName : EntityName;

    private sealed class DeferredWork : ScriptClosure
    {
        private readonly Action<SExclusiveContext> Work;

        public DeferredWork(Action<SExclusiveContext> Work)
        {
            this.Work = Work;
        }

        public override ulong Invoke(ulong Payload)
        {
            Work(new SExclusiveContext((IntPtr)(long)Payload));
            return 0;
        }
    }
}

// What a C# system running alone on the main thread receives, with the world in reach. A parallel system's SSystemContext has none.
public readonly unsafe partial struct SExclusiveContext
{
    // The native struct leads with its world pointer, refused to a parallel system holding one forged from its shared context.
    public CWorld World
    {
        get
        {
            if (!CanReachWorld())
            {
                throw new InvalidOperationException("The world is only in reach of an exclusive system or a deferred callback, not of a parallel system.");
            }
            return NativeObjectMarshal.FromHandle<CWorld>(*(IntPtr*)Self)!;
        }
    }

    private SSystemContext Shared => this;

    public FEntity Create(FName EntityName = default) => Shared.Create(EntityName);

    public FEntity Create(FVector3 Location, FName EntityName = default) => Shared.Create(Location, EntityName);

    public FEntity Create(FTransform Transform, FName EntityName = default) => Shared.Create(Transform, EntityName);

    public ref T Emplace<T>(FEntity Entity) where T : struct, IComponent<T> => ref Shared.Emplace<T>(Entity);

    public void Emplace<T>(FEntity Entity, in T Value) where T : struct, IComponent<T> => Shared.Emplace(Entity, in Value);

    public void Remove<T>(FEntity Entity) where T : struct, IComponent<T> => Shared.Remove<T>(Entity);

    public void Patch<T>(FEntity Entity, PatchFunc<T> Mutator) where T : struct, IComponent<T> => Shared.Patch(Entity, Mutator);

    public SystemPhysics Physics => new(this);

    public void Defer(Action<SExclusiveContext> Work) => Shared.Defer(Work);
}
