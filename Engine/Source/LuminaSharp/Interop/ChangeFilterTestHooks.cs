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

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_ChangeFilterRelease(IntPtr Handle)
    {
        GCHandle.FromIntPtr(Handle).Free();
    }
}
