using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Entities carrying any of these are left out of a view, mirroring ECS::TExclude.
public readonly struct Exclude
{
    internal readonly IntPtr[]? Tokens;

    private Exclude(IntPtr[] Tokens)
    {
        this.Tokens = Tokens;
    }

    public static Exclude None => default;

    public static Exclude Of<T1>() where T1 : struct, IComponent<T1> => new(ExcludeSet<T1>.Tokens);

    public static Exclude Of<T1, T2>() where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2>
        => new(ExcludeSet<T1, T2>.Tokens);

    public static Exclude Of<T1, T2, T3>() where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3>
        => new(ExcludeSet<T1, T2, T3>.Tokens);

    public static Exclude Of<T1, T2, T3, T4>() where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4>
        => new(ExcludeSet<T1, T2, T3, T4>.Tokens);

    private static class ExcludeSet<T1> where T1 : struct, IComponent<T1>
    {
        public static readonly IntPtr[] Tokens = { ComponentOps<T1>.Token };
    }

    private static class ExcludeSet<T1, T2> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2>
    {
        public static readonly IntPtr[] Tokens = { ComponentOps<T1>.Token, ComponentOps<T2>.Token };
    }

    private static class ExcludeSet<T1, T2, T3> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3>
    {
        public static readonly IntPtr[] Tokens = { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token };
    }

    private static class ExcludeSet<T1, T2, T3, T4> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4>
    {
        public static readonly IntPtr[] Tokens = { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, ComponentOps<T4>.Token };
    }
}

// The callback a view's ForEach runs per match, each component handed over in place.
public delegate void ViewFunc<T1>(FEntity Entity, ref T1 First) where T1 : struct, IComponent<T1>;
public delegate void ViewFunc<T1, T2>(FEntity Entity, ref T1 First, ref T2 Second) where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2>;
public delegate void ViewFunc<T1, T2, T3>(FEntity Entity, ref T1 First, ref T2 Second, ref T3 Third) where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3>;
public delegate void ViewFunc<T1, T2, T3, T4>(FEntity Entity, ref T1 First, ref T2 Second, ref T3 Third, ref T4 Fourth) where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4>;
public delegate void ViewFunc<T1, T2, T3, T4, T5>(FEntity Entity, ref T1 First, ref T2 Second, ref T3 Third, ref T4 Fourth, ref T5 Fifth) where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5>;
public delegate void ViewFunc<T1, T2, T3, T4, T5, T6>(FEntity Entity, ref T1 First, ref T2 Second, ref T3 Third, ref T4 Fourth, ref T5 Fifth, ref T6 Sixth) where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5> where T6 : struct, IComponent<T6>;
public delegate void ViewFunc<T1, T2, T3, T4, T5, T6, T7>(FEntity Entity, ref T1 First, ref T2 Second, ref T3 Third, ref T4 Fourth, ref T5 Fifth, ref T6 Sixth, ref T7 Seventh) where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5> where T6 : struct, IComponent<T6> where T7 : struct, IComponent<T7>;
public delegate void ViewFunc<T1, T2, T3, T4, T5, T6, T7, T8>(FEntity Entity, ref T1 First, ref T2 Second, ref T3 Third, ref T4 Fourth, ref T5 Fifth, ref T6 Sixth, ref T7 Seventh, ref T8 Eighth) where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5> where T6 : struct, IComponent<T6> where T7 : struct, IComponent<T7> where T8 : struct, IComponent<T8>;

// What a view shares with every row it hands out, in native memory so a row reaches it without a ref to the enumerator.
internal unsafe struct ViewHeader
{
    public const int MaxIncludes = 8;

    public ulong World;
    public int Includes;
    public uint WriteMask;
    public uint StampTick;
    public fixed long Tokens[MaxIncludes];
    public fixed long EpochSlots[MaxIncludes];
    public fixed uint ChunkEpochs[MaxIncludes];

    public TComponentRef<T> RefAt<T>(uint Entity, nint* Pointers, int Include) where T : struct, IComponent<T>
        => new(new NativeRef(Pointers[Include], World, Entity, (nint)Tokens[Include], (uint*)EpochSlots[Include]));
}

// Walks a native view one chunk per crossing, finding a row's components again before handing it out once their pool has changed.
internal unsafe struct ViewCursor : IDisposable
{
    private const int ChunkSize = 1024;

    private ViewHeader* Header;
    private uint* Entities;
    private nint* Pointers;
    private uint** Ticks;
    private IntPtr State;
    private int Count;
    private int Index;

    public ViewCursor(ulong World, ReadOnlySpan<IntPtr> IncludeTokens, Exclude Filter)
        : this(World, IncludeTokens.Length, IncludeTokens.Length >= 32 ? ~0u : (1u << IncludeTokens.Length) - 1u)
    {
        IntPtr* Include = stackalloc IntPtr[ViewHeader.MaxIncludes];
        for (int i = 0; i < IncludeTokens.Length; ++i)
        {
            Header->Tokens[i] = IncludeTokens[i];
            Include[i] = IncludeTokens[i];
        }

        IntPtr[]? Excluded = Filter.Tokens;
        fixed (IntPtr* Exclude = Excluded)
        {
            State = Native.ViewBegin(World, Include, IncludeTokens.Length, Exclude, Excluded?.Length ?? 0);
        }
        ReadEpochs();
    }

    // A query field's view, whose Write terms stamp a row only on mutable access and whose filters, when applied, keep only rows changed since its last run.
    public ViewCursor(SystemParamBlock* Query, bool bApplyFilters)
        : this((ulong)Query->World, Query->NumIncludes, Query->WriteMask)
    {
        for (int i = 0; i < Query->NumIncludes; ++i)
        {
            Header->Tokens[i] = Query->Includes[i];
        }
        Ticks = (uint**)NativeMemory.Alloc((nuint)(ChunkSize * Query->NumIncludes) * (nuint)sizeof(uint*));
        State = Native.QueryViewBegin((ulong)Query->World, (IntPtr*)Query->Includes, Query->NumIncludes, Query->WriteMask, (IntPtr*)Query->Excludes, Query->NumExcludes,
                                      (IntPtr*)Query->Requires, Query->NumRequires, (IntPtr*)Query->Filters, bApplyFilters ? Query->NumFilters : 0, Query->AddedMask, Query->LastRunTick,
                                      &Header->StampTick);
        ReadEpochs();
    }

    private ViewCursor(ulong World, int Includes, uint WriteMask)
    {
        Header = (ViewHeader*)NativeMemory.AllocZeroed((nuint)sizeof(ViewHeader));
        Entities = (uint*)NativeMemory.Alloc(ChunkSize * sizeof(uint));
        Pointers = (nint*)NativeMemory.Alloc((nuint)(ChunkSize * Includes) * (nuint)sizeof(nint));
        Ticks = null;
        State = IntPtr.Zero;
        Count = 0;
        Index = -1;
        Header->World = World;
        Header->Includes = Includes;
        Header->WriteMask = WriteMask;
    }

    private void ReadEpochs()
    {
        int Includes = Header->Includes;
        uint** Slots = stackalloc uint*[ViewHeader.MaxIncludes];
        if (State != IntPtr.Zero)
        {
            Native.ViewIncludeEpochs(State, Slots, Includes);
        }
        for (int i = 0; i < Includes; ++i)
        {
            Header->EpochSlots[i] = State != IntPtr.Zero ? (long)Slots[i] : 0;
        }
    }

    // The common row stays inside the gathered chunk with every pool unchanged, and only that much is inlined.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool MoveNext()
    {
        int Next = Index + 1;
        if (Next < Count && State != IntPtr.Zero && PoolsUnchanged())
        {
            Index = Next;
            return true;
        }
        return MoveNextSlow();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private bool PoolsUnchanged()
    {
        int Includes = Header->Includes;
        for (int i = 0; i < Includes; ++i)
        {
            uint* Slot = (uint*)Header->EpochSlots[i];
            if (Slot != null && *Slot != Header->ChunkEpochs[i])
            {
                return false;
            }
        }
        return true;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private bool MoveNextSlow()
    {
        while (State != IntPtr.Zero)
        {
            if (++Index >= Count)
            {
                Count = Native.ViewNextChunk(State, Entities, Pointers, Ticks, ChunkSize, Header->Includes);
                Index = 0;
                if (Count == 0)
                {
                    return false;
                }
                for (int i = 0; i < Header->Includes; ++i)
                {
                    uint* Slot = (uint*)Header->EpochSlots[i];
                    Header->ChunkEpochs[i] = Slot != null ? *Slot : 0;
                }
            }
            if (Revalidate())
            {
                return true;
            }
        }
        return false;
    }

    // Only after a pool changed since the chunk was gathered, so the common row costs one compare per include.
    private bool Revalidate()
    {
        int Includes = Header->Includes;
        bool bChanged = false;
        for (int i = 0; i < Includes && !bChanged; ++i)
        {
            uint* Slot = (uint*)Header->EpochSlots[i];
            bChanged = Slot != null && *Slot != Header->ChunkEpochs[i];
        }
        if (!bChanged)
        {
            return true;
        }

        uint EntityId = Entities[Index];
        nint* Row = Pointers + Index * Includes;
        for (int i = 0; i < Includes; ++i)
        {
            nint Token = (nint)Header->Tokens[i];
            // A tag has no payload to point at, so only its presence is checked.
            if (Row[i] == 0)
            {
                if (Native.HasComponent(Header->World, EntityId, Token) == 0)
                {
                    return false;
                }
                continue;
            }
            bool bWrite = (Header->WriteMask & (1u << i)) != 0;
            nint Found = bWrite ? Native.GetComponent(Header->World, EntityId, Token) : Native.GetComponentRead(Header->World, EntityId, Token);
            if (Found == 0)
            {
                return false;
            }
            Row[i] = Found;
            if (Ticks != null)
            {
                // GetComponent stamped a written row already, and the slot the chunk gathered may have moved with its pool.
                Ticks[Index * Includes + i] = null;
            }
        }
        return true;
    }

    public FEntity Entity => new(Entities[Index]);

    public ViewHeader* Shared => Header;

    public nint* Row => Pointers + Index * Header->Includes;

    public void* At(int Include) => (void*)Pointers[Index * Header->Includes + Include];

    public uint* TickAt(int Include) => Ticks != null ? Ticks[Index * Header->Includes + Include] : null;

    public TTerm Term<TTerm>(int Include) where TTerm : struct, IQueryTerm<TTerm> => TTerm.At((nint)At(Include), TickAt(Include), Header->StampTick);

    public void Dispose()
    {
        if (State != IntPtr.Zero)
        {
            Native.ViewEnd(State);
            State = IntPtr.Zero;
        }
        if (Header != null)
        {
            NativeMemory.Free(Header);
            NativeMemory.Free(Entities);
            NativeMemory.Free(Pointers);
            NativeMemory.Free(Ticks);
            Header = null;
            Entities = null;
            Pointers = null;
            Ticks = null;
        }
    }
}

// The matching entities, like ranging over a TView.
public struct ViewEntityEnumerator : IDisposable
{
    private ViewCursor Cursor;

    internal ViewEntityEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

    public FEntity Current => Cursor.Entity;

    public bool MoveNext() => Cursor.MoveNext();

    public void Dispose() => Cursor.Dispose();
}

public readonly struct View<T1> where T1 : struct, IComponent<T1>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1> View;

        internal EachRange(View<T1> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}

public readonly struct View<T1, T2> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token, ComponentOps<T2>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1, T2> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)), ref Unsafe.AsRef<T2>(Cursor.At(1)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);
        public ref T2 Second => ref Unsafe.AsRef<T2>((void*)Pointers[1]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First, out TComponentRef<T2> Second)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
            Second = Header->RefAt<T2>(this.Entity.Id, Pointers, 1);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1, T2> View;

        internal EachRange(View<T1, T2> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}

public readonly struct View<T1, T2, T3> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1, T2, T3> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)), ref Unsafe.AsRef<T2>(Cursor.At(1)), ref Unsafe.AsRef<T3>(Cursor.At(2)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);
        public ref T2 Second => ref Unsafe.AsRef<T2>((void*)Pointers[1]);
        public ref T3 Third => ref Unsafe.AsRef<T3>((void*)Pointers[2]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First, out TComponentRef<T2> Second, out TComponentRef<T3> Third)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
            Second = Header->RefAt<T2>(this.Entity.Id, Pointers, 1);
            Third = Header->RefAt<T3>(this.Entity.Id, Pointers, 2);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1, T2, T3> View;

        internal EachRange(View<T1, T2, T3> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}

public readonly struct View<T1, T2, T3, T4> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, ComponentOps<T4>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1, T2, T3, T4> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)), ref Unsafe.AsRef<T2>(Cursor.At(1)), ref Unsafe.AsRef<T3>(Cursor.At(2)), ref Unsafe.AsRef<T4>(Cursor.At(3)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);
        public ref T2 Second => ref Unsafe.AsRef<T2>((void*)Pointers[1]);
        public ref T3 Third => ref Unsafe.AsRef<T3>((void*)Pointers[2]);
        public ref T4 Fourth => ref Unsafe.AsRef<T4>((void*)Pointers[3]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First, out TComponentRef<T2> Second, out TComponentRef<T3> Third, out TComponentRef<T4> Fourth)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
            Second = Header->RefAt<T2>(this.Entity.Id, Pointers, 1);
            Third = Header->RefAt<T3>(this.Entity.Id, Pointers, 2);
            Fourth = Header->RefAt<T4>(this.Entity.Id, Pointers, 3);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1, T2, T3, T4> View;

        internal EachRange(View<T1, T2, T3, T4> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}

public readonly struct View<T1, T2, T3, T4, T5> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, ComponentOps<T4>.Token, ComponentOps<T5>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1, T2, T3, T4, T5> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)), ref Unsafe.AsRef<T2>(Cursor.At(1)), ref Unsafe.AsRef<T3>(Cursor.At(2)), ref Unsafe.AsRef<T4>(Cursor.At(3)), ref Unsafe.AsRef<T5>(Cursor.At(4)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);
        public ref T2 Second => ref Unsafe.AsRef<T2>((void*)Pointers[1]);
        public ref T3 Third => ref Unsafe.AsRef<T3>((void*)Pointers[2]);
        public ref T4 Fourth => ref Unsafe.AsRef<T4>((void*)Pointers[3]);
        public ref T5 Fifth => ref Unsafe.AsRef<T5>((void*)Pointers[4]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First, out TComponentRef<T2> Second, out TComponentRef<T3> Third, out TComponentRef<T4> Fourth, out TComponentRef<T5> Fifth)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
            Second = Header->RefAt<T2>(this.Entity.Id, Pointers, 1);
            Third = Header->RefAt<T3>(this.Entity.Id, Pointers, 2);
            Fourth = Header->RefAt<T4>(this.Entity.Id, Pointers, 3);
            Fifth = Header->RefAt<T5>(this.Entity.Id, Pointers, 4);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1, T2, T3, T4, T5> View;

        internal EachRange(View<T1, T2, T3, T4, T5> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}

public readonly struct View<T1, T2, T3, T4, T5, T6> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5> where T6 : struct, IComponent<T6>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, ComponentOps<T4>.Token, ComponentOps<T5>.Token, ComponentOps<T6>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1, T2, T3, T4, T5, T6> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)), ref Unsafe.AsRef<T2>(Cursor.At(1)), ref Unsafe.AsRef<T3>(Cursor.At(2)), ref Unsafe.AsRef<T4>(Cursor.At(3)), ref Unsafe.AsRef<T5>(Cursor.At(4)), ref Unsafe.AsRef<T6>(Cursor.At(5)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);
        public ref T2 Second => ref Unsafe.AsRef<T2>((void*)Pointers[1]);
        public ref T3 Third => ref Unsafe.AsRef<T3>((void*)Pointers[2]);
        public ref T4 Fourth => ref Unsafe.AsRef<T4>((void*)Pointers[3]);
        public ref T5 Fifth => ref Unsafe.AsRef<T5>((void*)Pointers[4]);
        public ref T6 Sixth => ref Unsafe.AsRef<T6>((void*)Pointers[5]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First, out TComponentRef<T2> Second, out TComponentRef<T3> Third, out TComponentRef<T4> Fourth, out TComponentRef<T5> Fifth, out TComponentRef<T6> Sixth)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
            Second = Header->RefAt<T2>(this.Entity.Id, Pointers, 1);
            Third = Header->RefAt<T3>(this.Entity.Id, Pointers, 2);
            Fourth = Header->RefAt<T4>(this.Entity.Id, Pointers, 3);
            Fifth = Header->RefAt<T5>(this.Entity.Id, Pointers, 4);
            Sixth = Header->RefAt<T6>(this.Entity.Id, Pointers, 5);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1, T2, T3, T4, T5, T6> View;

        internal EachRange(View<T1, T2, T3, T4, T5, T6> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}

public readonly struct View<T1, T2, T3, T4, T5, T6, T7> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5> where T6 : struct, IComponent<T6> where T7 : struct, IComponent<T7>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, ComponentOps<T4>.Token, ComponentOps<T5>.Token, ComponentOps<T6>.Token, ComponentOps<T7>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1, T2, T3, T4, T5, T6, T7> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)), ref Unsafe.AsRef<T2>(Cursor.At(1)), ref Unsafe.AsRef<T3>(Cursor.At(2)), ref Unsafe.AsRef<T4>(Cursor.At(3)), ref Unsafe.AsRef<T5>(Cursor.At(4)), ref Unsafe.AsRef<T6>(Cursor.At(5)), ref Unsafe.AsRef<T7>(Cursor.At(6)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);
        public ref T2 Second => ref Unsafe.AsRef<T2>((void*)Pointers[1]);
        public ref T3 Third => ref Unsafe.AsRef<T3>((void*)Pointers[2]);
        public ref T4 Fourth => ref Unsafe.AsRef<T4>((void*)Pointers[3]);
        public ref T5 Fifth => ref Unsafe.AsRef<T5>((void*)Pointers[4]);
        public ref T6 Sixth => ref Unsafe.AsRef<T6>((void*)Pointers[5]);
        public ref T7 Seventh => ref Unsafe.AsRef<T7>((void*)Pointers[6]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First, out TComponentRef<T2> Second, out TComponentRef<T3> Third, out TComponentRef<T4> Fourth, out TComponentRef<T5> Fifth, out TComponentRef<T6> Sixth, out TComponentRef<T7> Seventh)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
            Second = Header->RefAt<T2>(this.Entity.Id, Pointers, 1);
            Third = Header->RefAt<T3>(this.Entity.Id, Pointers, 2);
            Fourth = Header->RefAt<T4>(this.Entity.Id, Pointers, 3);
            Fifth = Header->RefAt<T5>(this.Entity.Id, Pointers, 4);
            Sixth = Header->RefAt<T6>(this.Entity.Id, Pointers, 5);
            Seventh = Header->RefAt<T7>(this.Entity.Id, Pointers, 6);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1, T2, T3, T4, T5, T6, T7> View;

        internal EachRange(View<T1, T2, T3, T4, T5, T6, T7> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}

public readonly struct View<T1, T2, T3, T4, T5, T6, T7, T8> where T1 : struct, IComponent<T1> where T2 : struct, IComponent<T2> where T3 : struct, IComponent<T3> where T4 : struct, IComponent<T4> where T5 : struct, IComponent<T5> where T6 : struct, IComponent<T6> where T7 : struct, IComponent<T7> where T8 : struct, IComponent<T8>
{
    private readonly ulong World;
    private readonly Exclude Filter;

    internal View(ulong World, Exclude Filter)
    {
        this.World = World;
        this.Filter = Filter;
    }

    private ViewCursor Open() => new(World, stackalloc IntPtr[] { ComponentOps<T1>.Token, ComponentOps<T2>.Token, ComponentOps<T3>.Token, ComponentOps<T4>.Token, ComponentOps<T5>.Token, ComponentOps<T6>.Token, ComponentOps<T7>.Token, ComponentOps<T8>.Token }, Filter);

    public ViewEntityEnumerator GetEnumerator() => new(Open());

    public EachRange Each() => new(this);

    public unsafe void ForEach(ViewFunc<T1, T2, T3, T4, T5, T6, T7, T8> Func)
    {
        using ViewCursor Cursor = Open();
        while (Cursor.MoveNext())
        {
            Func(Cursor.Entity, ref Unsafe.AsRef<T1>(Cursor.At(0)), ref Unsafe.AsRef<T2>(Cursor.At(1)), ref Unsafe.AsRef<T3>(Cursor.At(2)), ref Unsafe.AsRef<T4>(Cursor.At(3)), ref Unsafe.AsRef<T5>(Cursor.At(4)), ref Unsafe.AsRef<T6>(Cursor.At(5)), ref Unsafe.AsRef<T7>(Cursor.At(6)), ref Unsafe.AsRef<T8>(Cursor.At(7)));
        }
    }

    // One match with its components in place, so a field set through the row lands in storage. A ref is good until the next row.
    public readonly unsafe ref struct Row
    {
        private readonly ViewHeader* Header;
        private readonly nint* Pointers;

        internal Row(FEntity Entity, ViewHeader* Header, nint* Pointers)
        {
            this.Entity = Entity;
            this.Header = Header;
            this.Pointers = Pointers;
        }

        public FEntity Entity { get; }
        public ref T1 First => ref Unsafe.AsRef<T1>((void*)Pointers[0]);
        public ref T2 Second => ref Unsafe.AsRef<T2>((void*)Pointers[1]);
        public ref T3 Third => ref Unsafe.AsRef<T3>((void*)Pointers[2]);
        public ref T4 Fourth => ref Unsafe.AsRef<T4>((void*)Pointers[3]);
        public ref T5 Fifth => ref Unsafe.AsRef<T5>((void*)Pointers[4]);
        public ref T6 Sixth => ref Unsafe.AsRef<T6>((void*)Pointers[5]);
        public ref T7 Seventh => ref Unsafe.AsRef<T7>((void*)Pointers[6]);
        public ref T8 Eighth => ref Unsafe.AsRef<T8>((void*)Pointers[7]);

        // Tracked references rather than refs, since deconstructed values outlive the row.
        public void Deconstruct(out FEntity Entity, out TComponentRef<T1> First, out TComponentRef<T2> Second, out TComponentRef<T3> Third, out TComponentRef<T4> Fourth, out TComponentRef<T5> Fifth, out TComponentRef<T6> Sixth, out TComponentRef<T7> Seventh, out TComponentRef<T8> Eighth)
        {
            Entity = this.Entity;
            First = Header->RefAt<T1>(this.Entity.Id, Pointers, 0);
            Second = Header->RefAt<T2>(this.Entity.Id, Pointers, 1);
            Third = Header->RefAt<T3>(this.Entity.Id, Pointers, 2);
            Fourth = Header->RefAt<T4>(this.Entity.Id, Pointers, 3);
            Fifth = Header->RefAt<T5>(this.Entity.Id, Pointers, 4);
            Sixth = Header->RefAt<T6>(this.Entity.Id, Pointers, 5);
            Seventh = Header->RefAt<T7>(this.Entity.Id, Pointers, 6);
            Eighth = Header->RefAt<T8>(this.Entity.Id, Pointers, 7);
        }
    }

    public readonly struct EachRange
    {
        private readonly View<T1, T2, T3, T4, T5, T6, T7, T8> View;

        internal EachRange(View<T1, T2, T3, T4, T5, T6, T7, T8> View) => this.View = View;

        public EachEnumerator GetEnumerator() => new(View.Open());
    }

    public unsafe struct EachEnumerator : IDisposable
    {
        private ViewCursor Cursor;

        internal EachEnumerator(ViewCursor Cursor) => this.Cursor = Cursor;

        public Row Current => new(Cursor.Entity, Cursor.Shared, Cursor.Row);

        public bool MoveNext() => Cursor.MoveNext();

        public void Dispose() => Cursor.Dispose();
    }
}
