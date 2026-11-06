using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Lumina;

namespace LuminaSharp;

#pragma warning disable LUM0112
internal sealed class ChangeFilterTestSystem : CEntitySystem
#pragma warning restore LUM0112
{
    [Changed<SNameComponent>]
    public TQuery<Read<SNameComponent>> Renamed = new();

    [Added<STransformComponent>]
    public TQuery<Read<STransformComponent>> Spawned = new();

    [Changed<SNameComponent>]
    public TQuery<Read<STransformComponent>> PlacedAndRenamed = new();

    public TQuery<Read<SNameComponent>> Names = new();

    public TQuery<Write<SNameComponent>> NameWriters = new();
}

#pragma warning disable LUM0112
internal sealed class ParallelWriteTestSystem : CEntitySystem
#pragma warning restore LUM0112
{
    [Changed<STransformComponent>]
    public TQuery<Read<STransformComponent>> Moved = new();

    public TQuery<Write<STransformComponent>> Movers = new();
}

internal static unsafe class ChangeFilterTestHooks
{
    private static ChangeFilterTestSystem? Resolve(IntPtr Handle) => GCHandle.FromIntPtr(Handle).Target as ChangeFilterTestSystem;

    private static QueryParam Pick(ChangeFilterTestSystem Tested, int Which) => Which switch
    {
        0 => Tested.Renamed,
        1 => Tested.Spawned,
        _ => Tested.PlacedAndRenamed,
    };

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr Test_ChangeFilterBind(IntPtr NativeSystem)
    {
        var Tested = new ChangeFilterTestSystem();
        Tested.BindNativeHandle(NativeSystem);
        Tested.BindSystemParams();
        return GCHandle.ToIntPtr(GCHandle.Alloc(Tested));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ChangeFilterRun(IntPtr Handle, int Which, int bParallel)
    {
        if (Resolve(Handle) is not { } Tested)
        {
            return -1;
        }

        int Seen = 0;
        switch (Which)
        {
            case 0 when bParallel != 0:
                Tested.Renamed.ParallelForEach((FEntity Entity, Read<SNameComponent> Name) => Interlocked.Increment(ref Seen), 1);
                break;
            case 0:
                Tested.Renamed.ForEach((FEntity Entity, Read<SNameComponent> Name) => ++Seen);
                break;
            case 1 when bParallel != 0:
                Tested.Spawned.ParallelForEach((FEntity Entity, Read<STransformComponent> Transform) => Interlocked.Increment(ref Seen), 1);
                break;
            case 1:
                Tested.Spawned.ForEach((FEntity Entity, Read<STransformComponent> Transform) => ++Seen);
                break;
            default:
                Tested.PlacedAndRenamed.ForEach((FEntity Entity, Read<STransformComponent> Transform) => ++Seen);
                break;
        }
        return Seen;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static uint Test_ChangeFilterLastRunTick(IntPtr Handle, int Which)
        => Resolve(Handle) is { } Tested ? Pick(Tested, Which).Block->LastRunTick : 0;

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ChangeFilterIsEmpty(IntPtr Handle, int Which)
        => Resolve(Handle) is { } Tested && Pick(Tested, Which).IsEmpty ? 1 : 0;

    // Every read path a query offers, none of which may mark a row changed.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ChangeFilterReadAll(IntPtr Handle, uint Probe)
    {
        if (Resolve(Handle) is not { } Tested)
        {
            return 0;
        }

        int Visited = 0;
        Tested.Names.ForEach((FEntity Entity, Read<SNameComponent> Name) => ++Visited);
        Tested.Names.ParallelForEach((FEntity Entity, Read<SNameComponent> Name) => Interlocked.Increment(ref Visited), 1);
        bool bDirect = Tested.Names.TryGet(new FEntity(Probe), out Read<SNameComponent> _);
        bool bThroughFilter = Tested.PlacedAndRenamed.TryGet(new FEntity(Probe), out Read<SNameComponent> _);
        return bDirect && bThroughFilter ? Visited : -1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ChangeFilterWriteOne(IntPtr Handle, uint Target)
        => Resolve(Handle) is { } Tested && Tested.NameWriters.TryGet(new FEntity(Target), out Write<SNameComponent> _) ? 1 : 0;

    // Walks every name through Write terms, reading each and bypassing on each, and takes mutable access to Target alone.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ChangeFilterWriteWalk(IntPtr Handle, uint Target)
    {
        if (Resolve(Handle) is not { } Tested)
        {
            return 0;
        }

        int Visited = 0;
        void Visit(FEntity Entity, Write<SNameComponent> Name)
        {
            _ = Name.AsRead.Value;
            _ = Unsafe.AsPointer(ref Name.BypassChangeDetection());
            if (Entity.Id == Target)
            {
                _ = Unsafe.AsPointer(ref Name.Value);
            }
            Interlocked.Increment(ref Visited);
        }
        Tested.NameWriters.ForEach(Visit);
        Tested.NameWriters.ParallelForEach(Visit, 1);
        return Visited;
    }

    // Splits one Write walk over every transform by Id % 4 into a mutable write, a real SetIfChanged, an equal SetIfChanged and a read.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr Test_ChangeFilterParallelWrite(IntPtr NativeSystem, int* OutResults)
    {
        var Tested = new ParallelWriteTestSystem();
        Tested.BindNativeHandle(NativeSystem);
        Tested.BindSystemParams();

        int FirstRun = 0;
        Tested.Moved.ForEach((FEntity Entity, Read<STransformComponent> Transform) => ++FirstRun);

        int Visited = 0;
        int Stamped = 0;
        int Kept = 0;
        Tested.Movers.ParallelForEach((FEntity Entity, Write<STransformComponent> Transform) =>
        {
            switch (Entity.Id % 4)
            {
                case 0:
                    Transform.Value.LocalTransform.Location.X = 1.0f;
                    break;
                case 1:
                    Interlocked.Add(ref Stamped, Transform.SetIfChanged(static (in STransformComponent Row) => ref Row.LocalTransform.Location.X, 2.0f) ? 1 : 0);
                    break;
                case 2:
                    Interlocked.Add(ref Kept, Transform.SetIfChanged(static (in STransformComponent Row) => ref Row.LocalTransform.Location.X, 0.0f) ? 0 : 1);
                    break;
                default:
                    _ = Transform.AsRead.Value.LocalTransform.Location.X;
                    break;
            }
            Interlocked.Increment(ref Visited);
        });

        int Changed = 0;
        int Unwritten = 0;
        Tested.Moved.ForEach((FEntity Entity, Read<STransformComponent> Transform) =>
        {
            ++Changed;
            Unwritten += Entity.Id % 4 >= 2 ? 1 : 0;
        });

        int Rerun = 0;
        Tested.Moved.ForEach((FEntity Entity, Read<STransformComponent> Transform) => ++Rerun);

        OutResults[0] = FirstRun;
        OutResults[1] = Visited;
        OutResults[2] = Stamped;
        OutResults[3] = Kept;
        OutResults[4] = Changed;
        OutResults[5] = Unwritten;
        OutResults[6] = Rerun;
        return GCHandle.ToIntPtr(GCHandle.Alloc(Tested));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_ChangeFilterRelease(IntPtr Handle)
    {
        GCHandle.FromIntPtr(Handle).Free();
    }
}
