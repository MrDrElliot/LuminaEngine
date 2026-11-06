using System;
using System.Buffers;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Mirrors FManagedSystemParam in EntitySystem.cpp, which binds it to the world and asks it for work.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SystemParamBlock
{
    public const int MaxIncludes = 8;
    public const int MaxExcludes = 4;
    public const int MaxFilters = 4;
    public const int MaxRequires = 4;
    public const uint KindBound = 0;
    public const uint KindQuery = 1;
    public const uint KindEventReader = 2;

    public IntPtr World;
    public ulong Cursor;
    public uint Kind;
    public uint EventType;
    public int NumIncludes;
    public uint LastRunTick;
    public fixed long Includes[MaxIncludes];
    public uint WriteMask;
    public uint AddedMask;
    public int NumExcludes;
    public int NumFilters;
    public fixed long Excludes[MaxExcludes];
    public fixed long Filters[MaxFilters];
    public fixed long Requires[MaxRequires];
    public int NumRequires;
}

// A field of a C# entity system that declares its access by existing, the way a C++ system member does.
public abstract unsafe class SystemParam
{
    internal SystemParamBlock* Block;
    internal CWorld? BoundWorld;

    private protected SystemParam()
    {
    }

    ~SystemParam()
    {
        if (Block != null)
        {
            NativeMemory.Free(Block);
            Block = null;
        }
    }

    internal void Bind(CEntitySystem System, FieldInfo Field)
    {
        if (Block == null)
        {
            Block = (SystemParamBlock*)NativeMemory.AllocZeroed((nuint)sizeof(SystemParamBlock));
            Describe(Block, Field);
        }
        System.DeclareManagedParam((IntPtr)Block);
        BoundWorld = Block->World != IntPtr.Zero ? NativeObjectMarshal.FromHandle<CWorld>(Block->World) : null;
        OnBound();
        Declare(System);
    }

    private protected abstract void Describe(SystemParamBlock* Into, FieldInfo Field);

    private protected abstract void Declare(CEntitySystem System);

    private protected virtual void OnBound()
    {
    }

    private protected ulong WorldHandle => Block != null ? (ulong)Block->World : 0;

    private protected CMessageSubsystem RequireBus(CMessageSubsystem? Bus)
        => Bus ?? throw new InvalidOperationException($"{GetType().Name} is usable only once its system is configured in a world.");
}

// Finds a system's member fields once per type, weakly keyed so a reloaded script assembly can still unload.
internal static class SystemParams
{
    private static readonly ConditionalWeakTable<Type, FieldInfo[]> FieldsByType = new();

    internal static FieldInfo[] FieldsOf(Type SystemType) => FieldsByType.GetValue(SystemType, Discover);

    private static FieldInfo[] Discover(Type SystemType)
    {
        var Found = new List<FieldInfo>();
        for (Type? At = SystemType; At != null && At != typeof(CEntitySystem); At = At.BaseType)
        {
            foreach (FieldInfo Field in At.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
            {
                if (typeof(SystemParam).IsAssignableFrom(Field.FieldType))
                {
                    Found.Add(Field);
                }
            }
        }
        return Found.ToArray();
    }
}

// One component a query names and how it may touch it: Read<T> or Write<T>.
public interface IQueryTerm<TSelf> where TSelf : struct, IQueryTerm<TSelf>
{
    [EditorBrowsable(EditorBrowsableState.Never)]
    static abstract IntPtr Token { get; }

    [EditorBrowsable(EditorBrowsableState.Never)]
    static abstract bool IsWrite { get; }

    [EditorBrowsable(EditorBrowsableState.Never)]
    static abstract string ComponentName { get; }

    [EditorBrowsable(EditorBrowsableState.Never)]
    static abstract unsafe TSelf At(nint Component, uint* Tick, uint Now);
}

// A query term that reads T in place, so the system runs beside any other reader of T. Its Value, T.ReadOnly, is a generated extension.
public readonly unsafe struct Read<T> : IQueryTerm<Read<T>> where T : struct, IComponent<T>
{
    private readonly nint Pointer;

    internal Read(nint Pointer)
    {
        this.Pointer = Pointer;
    }

    [EditorBrowsable(EditorBrowsableState.Never)]
    public nint Address => Pointer;

    static IntPtr IQueryTerm<Read<T>>.Token => ComponentOps<T>.Token;
    static bool IQueryTerm<Read<T>>.IsWrite => false;
    static string IQueryTerm<Read<T>>.ComponentName => T.NativeName;
    static Read<T> IQueryTerm<Read<T>>.At(nint Component, uint* Tick, uint Now) => new(Component);
}

// A query term that writes T in place, so the system runs alone against anything else touching T. Like ECS::TMut, only mutable access stamps the row.
public readonly unsafe struct Write<T> : IQueryTerm<Write<T>> where T : struct, IComponent<T>
{
    private readonly nint Pointer;
    private readonly uint* Tick;
    private readonly uint Now;

    private Write(nint Pointer, uint* Tick, uint Now)
    {
        this.Pointer = Pointer;
        this.Tick = Tick;
        this.Now = Now;
    }

    public ref T Value
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get
        {
            Stamp();
            return ref BypassChangeDetection();
        }
    }

    // The same row as a Read<T>, which never stamps.
    public Read<T> AsRead => new(Pointer);

    // Returns whether it wrote, so an unchanged value leaves Changed<T> quiet, for example Transform.SetIfChanged(static (in STransformComponent Row) => ref Row.LocalTransform.Location.X, 1.0f).
    public bool SetIfChanged<TField>(FieldOf<T, TField> Field, TField NewValue)
    {
        ref TField Dest = ref Field(in BypassChangeDetection());
        if (EqualityComparer<TField>.Default.Equals(Dest, NewValue))
        {
            return false;
        }
        Dest = NewValue;
        Stamp();
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void Stamp()
    {
        if (Tick != null)
        {
            *Tick = Now;
        }
    }

    // For a write no Changed<T> reader should see.
    public ref T BypassChangeDetection() => ref Unsafe.AsRef<T>((void*)Pointer);

    static IntPtr IQueryTerm<Write<T>>.Token => ComponentOps<T>.Token;
    static bool IQueryTerm<Write<T>>.IsWrite => true;
    static string IQueryTerm<Write<T>>.ComponentName => T.NativeName;
    static Write<T> IQueryTerm<Write<T>>.At(nint Component, uint* Tick, uint Now) => new(Component, Tick, Now);
}

internal interface IQueryExclude
{
    IntPtr Token { get; }
}

internal interface IQueryRequire
{
    IntPtr Token { get; }
}

internal interface IQueryTickFilter
{
    IntPtr Token { get; }
    bool bAdded { get; }
}

// Leaves entities carrying T out of the query field it marks, like ECS::Without.
[AttributeUsage(AttributeTargets.Field, AllowMultiple = true)]
public sealed class WithoutAttribute<T> : Attribute, IQueryExclude where T : struct, IComponent<T>
{
    IntPtr IQueryExclude.Token => ComponentOps<T>.Token;
}

// Keeps only entities carrying T without reading it, like ECS::With, so the field runs beside systems writing T.
[AttributeUsage(AttributeTargets.Field, AllowMultiple = true)]
public sealed class WithAttribute<T> : Attribute, IQueryRequire where T : struct, IComponent<T>
{
    IntPtr IQueryRequire.Token => ComponentOps<T>.Token;
}

// Keeps only entities whose T was written since the field's last full run, like ECS::Changed. T need not be a term, and reads as one.
[AttributeUsage(AttributeTargets.Field, AllowMultiple = true)]
public sealed class ChangedAttribute<T> : Attribute, IQueryTickFilter where T : struct, IComponent<T>
{
    IntPtr IQueryTickFilter.Token => ComponentOps<T>.Token;
    bool IQueryTickFilter.bAdded => false;
}

// Keeps only entities that gained T since the field's last full run, like ECS::Added.
[AttributeUsage(AttributeTargets.Field, AllowMultiple = true)]
public sealed class AddedAttribute<T> : Attribute, IQueryTickFilter where T : struct, IComponent<T>
{
    IntPtr IQueryTickFilter.Token => ComponentOps<T>.Token;
    bool IQueryTickFilter.bAdded => true;
}

public delegate ref TField FieldOf<T, TField>(in T Row);

public delegate void QueryFunc<T1>(FEntity Entity, T1 First);
public delegate void QueryFunc<T1, T2>(FEntity Entity, T1 First, T2 Second);
public delegate void QueryFunc<T1, T2, T3>(FEntity Entity, T1 First, T2 Second, T3 Third);
public delegate void QueryFunc<T1, T2, T3, T4>(FEntity Entity, T1 First, T2 Second, T3 Third, T4 Fourth);

internal readonly record struct QueryTermInfo(IntPtr Token, bool bWrite, string Name)
{
    public static QueryTermInfo Of<T>() where T : struct, IQueryTerm<T> => new(T.Token, T.IsWrite, T.ComponentName);
}

// What every arity of TQuery shares: the view it opens, the access it declares and the parallel split.
public abstract unsafe class QueryParam : SystemParam
{
    private IntPtr[] Tokens = Array.Empty<IntPtr>();
    private IntPtr[] ExcludeTokens = Array.Empty<IntPtr>();
    private IntPtr[] RequireTokens = Array.Empty<IntPtr>();
    private IntPtr[] FilterTokens = Array.Empty<IntPtr>();

    private protected QueryParam()
    {
    }

    private protected abstract QueryTermInfo[] Terms { get; }

    private protected override void Describe(SystemParamBlock* Into, FieldInfo Field)
    {
        QueryTermInfo[] Named = Terms;
        Tokens = new IntPtr[Named.Length];
        Into->Kind = SystemParamBlock.KindQuery;
        Into->NumIncludes = Named.Length;
        for (int Index = 0; Index < Named.Length; ++Index)
        {
            Tokens[Index] = Named[Index].Token;
            Into->Includes[Index] = Named[Index].Token;
            Into->WriteMask |= Named[Index].bWrite ? 1u << Index : 0u;
        }

        ExcludeTokens = Field.GetCustomAttributes().OfType<IQueryExclude>().Select(Without => Without.Token).Distinct().ToArray();
        if (ExcludeTokens.Length > SystemParamBlock.MaxExcludes)
        {
            throw new InvalidOperationException($"Query field '{Field.Name}' excludes {ExcludeTokens.Length} components; at most {SystemParamBlock.MaxExcludes} are supported.");
        }
        Into->NumExcludes = ExcludeTokens.Length;
        for (int Index = 0; Index < ExcludeTokens.Length; ++Index)
        {
            Into->Excludes[Index] = ExcludeTokens[Index];
        }

        RequireTokens = Field.GetCustomAttributes().OfType<IQueryRequire>().Select(With => With.Token).Distinct().ToArray();
        if (RequireTokens.Length > SystemParamBlock.MaxRequires)
        {
            throw new InvalidOperationException($"Query field '{Field.Name}' requires {RequireTokens.Length} components through With; at most {SystemParamBlock.MaxRequires} are supported.");
        }
        if (RequireTokens.Any(Token => Tokens.Contains(Token) || ExcludeTokens.Contains(Token)))
        {
            throw new InvalidOperationException($"Query field '{Field.Name}' names a With component that is already a term or a Without.");
        }
        Into->NumRequires = RequireTokens.Length;
        for (int Index = 0; Index < RequireTokens.Length; ++Index)
        {
            Into->Requires[Index] = RequireTokens[Index];
        }

        IQueryTickFilter[] Filters = Field.GetCustomAttributes().OfType<IQueryTickFilter>().DistinctBy(Filter => (Filter.Token, Filter.bAdded)).ToArray();
        if (Filters.Length > SystemParamBlock.MaxFilters)
        {
            throw new InvalidOperationException($"Query field '{Field.Name}' has {Filters.Length} Changed or Added filters; at most {SystemParamBlock.MaxFilters} are supported.");
        }
        FilterTokens = new IntPtr[Filters.Length];
        Into->NumFilters = Filters.Length;
        for (int Index = 0; Index < Filters.Length; ++Index)
        {
            FilterTokens[Index] = Filters[Index].Token;
            Into->Filters[Index] = Filters[Index].Token;
            Into->AddedMask |= Filters[Index].bAdded ? 1u << Index : 0u;
        }
    }

    // DeclareManagedParam declares the terms and filters from the block, as TQuery declares its own.
    private protected override void Declare(CEntitySystem System)
    {
    }

    private bool HasFilters => FilterTokens.Length > 0;

    private protected ViewCursor Open(bool bApplyFilters = true) => Block != null ? new ViewCursor(Block, bApplyFilters) : new ViewCursor(0, Tokens, default);

    // After the visits, so their writes carry this run's tick and are not reported back to the next run.
    private protected void CompleteRun()
    {
        if (HasFilters && Block != null)
        {
            Block->LastRunTick = Native.AdvanceChangeTick();
        }
    }

    public bool IsEmpty
    {
        get
        {
            using ViewCursor Cursor = Open(bApplyFilters: false);
            return !Cursor.MoveNext();
        }
    }

    public bool Contains(FEntity Entity)
    {
        ulong World = WorldHandle;
        if (World == 0)
        {
            return false;
        }
        foreach (IntPtr Token in Tokens)
        {
            if (Native.HasComponent(World, Entity.Id, Token) == 0)
            {
                return false;
            }
        }
        foreach (IntPtr Token in ExcludeTokens)
        {
            if (Native.HasComponent(World, Entity.Id, Token) != 0)
            {
                return false;
            }
        }
        foreach (IntPtr Token in RequireTokens)
        {
            if (Native.HasComponent(World, Entity.Id, Token) == 0)
            {
                return false;
            }
        }
        return true;
    }

    // Random access, only to the components the query names and only as it names them.
    public bool TryGet<TTerm>(FEntity Entity, out TTerm Term) where TTerm : struct, IQueryTerm<TTerm>
    {
        RequireTerm<TTerm>();
        Term = default;
        ulong World = WorldHandle;
        if (World == 0)
        {
            return false;
        }
        nint Found = TTerm.IsWrite ? Native.GetComponent(World, Entity.Id, TTerm.Token) : Native.GetComponentRead(World, Entity.Id, TTerm.Token);
        if (Found == 0 && Native.HasComponent(World, Entity.Id, TTerm.Token) == 0)
        {
            return false;
        }
        Term = TTerm.At(Found, null, 0);
        return true;
    }

    public TTerm Get<TTerm>(FEntity Entity) where TTerm : struct, IQueryTerm<TTerm>
        => TryGet(Entity, out TTerm Term) ? Term : throw new InvalidOperationException($"{Entity} has no {TTerm.ComponentName}.");

    private void RequireTerm<TTerm>() where TTerm : struct, IQueryTerm<TTerm>
    {
        foreach (QueryTermInfo Term in Terms)
        {
            if (Term.Token == TTerm.Token && (Term.bWrite || !TTerm.IsWrite))
            {
                return;
            }
        }
        if (!TTerm.IsWrite && Array.IndexOf(FilterTokens, TTerm.Token) >= 0)
        {
            return;
        }
        throw new InvalidOperationException($"This query does not name {TTerm.ComponentName} with {(TTerm.IsWrite ? "write" : "read")} access.");
    }

    // The rows a parallel walk gathered, each include's component beside the tick slot its Write term stamps.
    private protected readonly struct GatheredRows(nint[] Pointers, nint[] Ticks, uint Now)
    {
        public TTerm At<TTerm>(int Slot) where TTerm : struct, IQueryTerm<TTerm> => TTerm.At(Pointers[Slot], (uint*)Ticks[Slot], Now);
    }

    private protected delegate void RowFunc(FEntity Entity, GatheredRows Rows, int Row);

    private protected void RunParallel(int MinRange, RowFunc Body)
    {
        int Includes = Tokens.Length;
        uint[] Entities = ArrayPool<uint>.Shared.Rent(256);
        nint[] Pointers = ArrayPool<nint>.Shared.Rent(256 * Includes);
        nint[] Ticks = ArrayPool<nint>.Shared.Rent(256 * Includes);
        uint Now = 0;
        int Count = 0;
        try
        {
            using (ViewCursor Cursor = Open())
            {
                while (Cursor.MoveNext())
                {
                    if (Count == Entities.Length)
                    {
                        Grow(ref Entities, Count * 2);
                        Grow(ref Pointers, Count * 2 * Includes);
                        Grow(ref Ticks, Count * 2 * Includes);
                    }
                    Entities[Count] = Cursor.Entity.Id;
                    for (int Include = 0; Include < Includes; ++Include)
                    {
                        Pointers[Count * Includes + Include] = (nint)Cursor.At(Include);
                        Ticks[Count * Includes + Include] = (nint)Cursor.TickAt(Include);
                    }
                    ++Count;
                }
                Now = Cursor.Shared->StampTick;
            }

            GatheredRows Rows = new(Pointers, Ticks, Now);

            int Range = Math.Max(MinRange, 1);
            if (Count <= Range)
            {
                for (int Row = 0; Row < Count; ++Row)
                {
                    Body(new FEntity(Entities[Row]), Rows, Row * Includes);
                }
            }
            else
            {
                CTaskLibrary.ParallelFor((uint)Count, (uint)Range, ScriptCallback.Bind(new RowBody(Body, Entities, Rows, Includes)), 1);
            }
            CompleteRun();
        }
        finally
        {
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<nint>.Shared.Return(Pointers);
            ArrayPool<nint>.Shared.Return(Ticks);
        }
    }

    private static void Grow<T>(ref T[] Array, int Size)
    {
        T[] Larger = ArrayPool<T>.Shared.Rent(Size);
        Array.AsSpan().CopyTo(Larger);
        ArrayPool<T>.Shared.Return(Array);
        Array = Larger;
    }

    private sealed class RowBody : ScriptClosure
    {
        private readonly RowFunc Body;
        private readonly uint[] Entities;
        private readonly GatheredRows Rows;
        private readonly int Includes;

        public RowBody(RowFunc Body, uint[] Entities, GatheredRows Rows, int Includes)
        {
            this.Body = Body;
            this.Entities = Entities;
            this.Rows = Rows;
            this.Includes = Includes;
        }

        public override ulong Invoke(ulong Payload)
        {
            for (int Row = (int)(uint)Payload, End = (int)(Payload >> 32); Row < End; ++Row)
            {
                Body(new FEntity(Entities[Row]), Rows, Row * Includes);
            }
            return 0;
        }
    }
}

// A system field over every entity holding the named components, for example TQuery<Write<SVelocity>, Read<STransformComponent>>.
public sealed class TQuery<T1> : QueryParam where T1 : struct, IQueryTerm<T1>
{
    private static readonly QueryTermInfo[] TermList = { QueryTermInfo.Of<T1>() };

    private protected override QueryTermInfo[] Terms => TermList;

    public unsafe void ForEach(QueryFunc<T1> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, Cursor.Term<T1>(0));
        }
        CompleteRun();
    }

    public void ParallelForEach(QueryFunc<T1> Func, int MinRange = 64)
        => RunParallel(MinRange, (Entity, Rows, Row) => Func(Entity, Rows.At<T1>(Row)));
}

public sealed class TQuery<T1, T2> : QueryParam where T1 : struct, IQueryTerm<T1> where T2 : struct, IQueryTerm<T2>
{
    private static readonly QueryTermInfo[] TermList = { QueryTermInfo.Of<T1>(), QueryTermInfo.Of<T2>() };

    private protected override QueryTermInfo[] Terms => TermList;

    public unsafe void ForEach(QueryFunc<T1, T2> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, Cursor.Term<T1>(0), Cursor.Term<T2>(1));
        }
        CompleteRun();
    }

    public void ParallelForEach(QueryFunc<T1, T2> Func, int MinRange = 64)
        => RunParallel(MinRange, (Entity, Rows, Row) => Func(Entity, Rows.At<T1>(Row), Rows.At<T2>(Row + 1)));
}

public sealed class TQuery<T1, T2, T3> : QueryParam where T1 : struct, IQueryTerm<T1> where T2 : struct, IQueryTerm<T2> where T3 : struct, IQueryTerm<T3>
{
    private static readonly QueryTermInfo[] TermList = { QueryTermInfo.Of<T1>(), QueryTermInfo.Of<T2>(), QueryTermInfo.Of<T3>() };

    private protected override QueryTermInfo[] Terms => TermList;

    public unsafe void ForEach(QueryFunc<T1, T2, T3> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, Cursor.Term<T1>(0), Cursor.Term<T2>(1), Cursor.Term<T3>(2));
        }
        CompleteRun();
    }

    public void ParallelForEach(QueryFunc<T1, T2, T3> Func, int MinRange = 64)
        => RunParallel(MinRange, (Entity, Rows, Row) => Func(Entity, Rows.At<T1>(Row), Rows.At<T2>(Row + 1), Rows.At<T3>(Row + 2)));
}

public sealed class TQuery<T1, T2, T3, T4> : QueryParam
    where T1 : struct, IQueryTerm<T1> where T2 : struct, IQueryTerm<T2> where T3 : struct, IQueryTerm<T3> where T4 : struct, IQueryTerm<T4>
{
    private static readonly QueryTermInfo[] TermList = { QueryTermInfo.Of<T1>(), QueryTermInfo.Of<T2>(), QueryTermInfo.Of<T3>(), QueryTermInfo.Of<T4>() };

    private protected override QueryTermInfo[] Terms => TermList;

    public unsafe void ForEach(QueryFunc<T1, T2, T3, T4> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, Cursor.Term<T1>(0), Cursor.Term<T2>(1), Cursor.Term<T3>(2), Cursor.Term<T4>(3));
        }
        CompleteRun();
    }

    public void ParallelForEach(QueryFunc<T1, T2, T3, T4> Func, int MinRange = 64)
        => RunParallel(MinRange, (Entity, Rows, Row) => Func(Entity, Rows.At<T1>(Row), Rows.At<T2>(Row + 1), Rows.At<T3>(Row + 2), Rows.At<T4>(Row + 3)));
}

// A system field that reads every T sent since it last read, like ECS::TEventReader. Read and ReadViews come from SystemEvents.
public sealed unsafe class TEventReader<T> : SystemParam where T : struct, IEvent
{
    private CMessageSubsystem? Bus;

    private protected override void Describe(SystemParamBlock* Into, FieldInfo Field)
    {
        Into->Kind = SystemParamBlock.KindEventReader;
        Into->EventType = EventType<T>.Id;
    }

    private protected override void OnBound() => Bus = BoundWorld?.MessageBus;

    private protected override void Declare(CEntitySystem System) => System.DeclareRead(EventType<T>.Name);

    public bool HasUnread() => Bus != null && Bus.NumUnreadEvents(EventType<T>.Id, Block->Cursor) != 0;

    internal EventRuns ReadRuns()
    {
        EventRuns Runs = default;
        RequireBus(Bus).ReadEvents(EventType<T>.Id, (IntPtr)(&Block->Cursor), (IntPtr)(&Runs));
        return Runs;
    }
}

// A system field that sends T, readable after the flush that follows this system's batch, like ECS::TEventWriter.
public sealed unsafe class TEventWriter<T> : SystemParam where T : unmanaged, IEvent
{
    private CMessageSubsystem? Bus;

    private protected override void Describe(SystemParamBlock* Into, FieldInfo Field)
    {
        Into->Kind = SystemParamBlock.KindBound;
    }

    private protected override void OnBound() => Bus = BoundWorld?.MessageBus;

    private protected override void Declare(CEntitySystem System) => System.DeclareWrite(EventType<T>.Name);

    public void Send(in T Event)
    {
        RequireValueEvent();
        fixed (T* Pointer = &Event)
        {
            RequireBus(Bus).SendEvents(EventType<T>.Id, (IntPtr)Pointer, 1);
        }
    }

    public void Send(ReadOnlySpan<T> Events)
    {
        RequireValueEvent();
        if (Events.IsEmpty)
        {
            return;
        }
        fixed (T* Pointer = Events)
        {
            RequireBus(Bus).SendEvents(EventType<T>.Id, (IntPtr)Pointer, (uint)Events.Length);
        }
    }

    private static void RequireValueEvent()
    {
        if (EventType<T>.bView)
        {
            throw new InvalidOperationException($"{typeof(T).Name} holds native memory, so send it from C++.");
        }
    }
}

public static unsafe class SystemEvents
{
    // Every T sent since Reader last read, oldest first, straight from native memory.
    public static EventSpan<T> Read<T>(this TEventReader<T> Reader) where T : unmanaged, IEvent
    {
        if (EventType<T>.bView)
        {
            throw new InvalidOperationException($"{typeof(T).Name} holds native memory, so read it with ReadViews.");
        }
        EventRuns Runs = Reader.ReadRuns();
        return new EventSpan<T>(new ReadOnlySpan<T>((void*)Runs.First.Data, (int)Runs.First.Count),
                                new ReadOnlySpan<T>((void*)Runs.Second.Data, (int)Runs.Second.Count));
    }

    // Read for an event that holds strings or object references, each one a view over the queued native value.
    public static EventViews<T> ReadViews<T>(this TEventReader<T> Reader) where T : struct, INativeStruct<T>, IEvent
        => new(Reader.ReadRuns());
}
