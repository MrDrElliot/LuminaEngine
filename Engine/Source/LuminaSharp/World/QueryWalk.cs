using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Mirrors FManagedRowProbe in DotNetView.cpp, one pool's lookup, payload and stamp layout, good while its layout epoch holds.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct RowProbe
{
    public const uint InvalidDense = EntityIndexMask;
    public const uint TombstoneVersion = (1u << (32 - EntityIndexBits)) - 1u;
    public const int EntityIndexBits = 20;

    private const uint EntityIndexMask = (1u << EntityIndexBits) - 1u;
    private const int SparsePageShift = 12;
    private const uint SparsePageMask = (1u << SparsePageShift) - 1u;

    private readonly uint** Pages;
    private readonly ulong PageCount;
    private readonly byte** PayloadPages;
    private readonly byte* Packed;
    private readonly uint* TickBase;
    private readonly uint* EpochSlot;
    private readonly uint ElementSize;
    private readonly int PageShift;
    private readonly uint PageMask;
    private readonly uint TickStride;
    private readonly uint bPaged;
    private readonly uint Epoch;

    public bool Moved
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => *EpochSlot != Epoch;
    }

    // The same test as FSparseSet::FindLiveDense, a slot whose version differs reading as absent.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public uint FindLiveDense(uint Entity)
    {
        uint Index = Entity & EntityIndexMask;
        uint Page = Index >> SparsePageShift;
        if (Page >= PageCount)
        {
            return InvalidDense;
        }
        uint* Slots = Pages[Page];
        if (Slots == null)
        {
            return InvalidDense;
        }
        uint Slot = Slots[Index & SparsePageMask];
        return (Slot >> EntityIndexBits) == (Entity >> EntityIndexBits) ? Slot & EntityIndexMask : InvalidDense;
    }

    // A packed pool arrives as one page, and a tag as a null one, so every pool resolves the same way.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public nint PayloadAt(uint Dense) => (nint)(PayloadPages[Dense >> PageShift] + (nuint)(Dense & PageMask) * ElementSize);

    // Null for a pool that does not stamp, whose base is null with no stride.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public uint* TickAt(uint Dense) => TickBase + Dense * TickStride;
}

// A query's walk that finds each row itself from its pools' probes, so nothing is gathered and read back. Probes are fetched again whenever a callback moved a pool.
internal unsafe struct QueryWalk : IDisposable
{
    private const int MaxProbes = SystemParamBlock.MaxIncludes + SystemParamBlock.MaxExcludes + SystemParamBlock.MaxRequires;

    private IntPtr State;
    private RowProbe* ProbeData;
    private uint* Generation;
    private uint SeenGeneration;
    private int NumProbes;
    private int NumExcludes;
    private int NumRequires;
    private int Includes;
    private int Driver;

    public readonly uint* Snapshot;
    public readonly int Count;
    public readonly uint Now;

    public QueryWalk(SystemParamBlock* Query, bool bApplyFilters)
    {
        uint StampTick = 0;
        State = Native.QueryViewBegin((ulong)Query->World, (IntPtr*)Query->Includes, Query->NumIncludes, Query->WriteMask, (IntPtr*)Query->Excludes, Query->NumExcludes,
                                      (IntPtr*)Query->Requires, Query->NumRequires, (IntPtr*)Query->Filters, bApplyFilters ? Query->NumFilters : 0, Query->AddedMask, Query->LastRunTick,
                                      &StampTick);
        Now = StampTick;
        Includes = Query->NumIncludes;
        ProbeData = (RowProbe*)NativeMemory.Alloc((nuint)(MaxProbes * sizeof(RowProbe)));
        uint* First = null;
        int DriverInclude = -1;
        Count = State != IntPtr.Zero ? Native.ViewSnapshot(State, &First, &DriverInclude) : 0;
        Snapshot = First;
        Driver = DriverInclude;
        NumProbes = 0;
        NumExcludes = 0;
        NumRequires = 0;
        Generation = null;
        SeenGeneration = 0;
        Refresh();
        if (NumProbes < Includes)
        {
            Count = 0;
        }
    }

    public RowProbe* Probes => ProbeData;

    // The include whose dense slot is the snapshot slot, or -1 once its pool moved or when the snapshot is not its dense array.
    public int InPlaceInclude => Driver;

    // Moves whenever any pool anywhere moved, so a walk that saw Seen here has nothing to check.
    public uint* GenerationSlot => Generation;

    public uint Seen => SeenGeneration;

    public bool HasMembershipProbes => NumExcludes + NumRequires != 0;

    // After the generation moved, checks each probe, and true once they were all fetched again.
    [MethodImpl(MethodImplOptions.NoInlining)]
    public bool Revalidate()
    {
        uint Current = *Generation;
        for (int i = 0; i < NumProbes; ++i)
        {
            if (ProbeData[i].Moved)
            {
                Driver = -1;
                Refresh();
                return true;
            }
        }
        SeenGeneration = Current;
        return false;
    }

    // The exclude and With pools, which only say whether Entity is in the walk.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool PassesMembership(uint Entity)
    {
        RowProbe* Excluded = ProbeData + Includes;
        for (int e = 0; e < NumExcludes; ++e)
        {
            if (Excluded[e].FindLiveDense(Entity) != RowProbe.InvalidDense)
            {
                return false;
            }
        }
        RowProbe* Required = Excluded + NumExcludes;
        for (int r = 0; r < NumRequires; ++r)
        {
            if (Required[r].FindLiveDense(Entity) == RowProbe.InvalidDense)
            {
                return false;
            }
        }
        return true;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private void Refresh()
    {
        int Excludes = 0;
        int Requires = 0;
        uint* GenerationSlot = null;
        uint Seen = 0;
        NumProbes = State != IntPtr.Zero ? Native.ViewRowProbes(State, ProbeData, MaxProbes, &Excludes, &Requires, &GenerationSlot, &Seen) : 0;
        NumExcludes = Excludes;
        NumRequires = Requires;
        Generation = GenerationSlot;
        SeenGeneration = Seen;
    }

    public void Dispose()
    {
        if (State != IntPtr.Zero)
        {
            Native.ViewEnd(State);
            State = IntPtr.Zero;
        }
        if (ProbeData != null)
        {
            NativeMemory.Free(ProbeData);
            ProbeData = null;
        }
    }
}
