using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// A system body run by hand against a native SSystemContext, the way the scheduler hands one to OnUpdate.
internal static unsafe class SystemContextTestHooks
{
    private static readonly int[] Deferred = new int[4];

    private static WeakReference? DiscardedMarker;

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SystemContextRun(IntPtr ContextAddress, uint Existing, uint Doomed, IntPtr SourceLifetime, uint CopyTarget, int* Out)
    {
        try
        {
            Array.Clear(Deferred);
            var Context = new SSystemContext(ContextAddress);

            FEntity Created = Context.Create(new FVector3(1.0f, 2.0f, 3.0f), "CreatedByCSharp");
            Context.SetEntityLifetime(Created, 3.0f);
            Context.Emplace<SLifetimeComponent>(new FEntity(Existing)).Lifetime = 5.0f;
            Context.Emplace<SDisabledTag>(new FEntity(Existing));
            Context.Remove<SNameComponent>(new FEntity(Existing));
            Context.Emplace(new FEntity(CopyTarget), in Unsafe.AsRef<SLifetimeComponent>((void*)SourceLifetime));
            Context.Destroy(new FEntity(Doomed));
            Context.Patch<STransformComponent>(new FEntity(Existing), (ref STransformComponent Transform) => Transform.LocalTransform.Location.Z = 4.0f);

            Context.Defer(Exclusive =>
            {
                Deferred[0] = 1;
                Deferred[1] = Exclusive.World is not null ? 1 : 0;
                FEntity Spawned = Exclusive.Create();
                Deferred[2] = Exclusive.IsValidEntity(Spawned) ? 1 : 0;
                Deferred[3] = (int)Spawned.Id;
            });

            int WorldRefused = 0;
            try
            {
                _ = new SExclusiveContext(Context.Ref).World;
            }
            catch (InvalidOperationException)
            {
                WorldRefused = 1;
            }

            Out[0] = (int)Created.Id;
            Out[1] = WorldRefused;
            return 1;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 0;
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_SystemContextDeferred(int* Out)
    {
        for (int Index = 0; Index < Deferred.Length; ++Index)
        {
            Out[Index] = Deferred[Index];
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_SystemContextDeferDiscarded(IntPtr ContextAddress)
    {
        DeferHoldingMarker(new SSystemContext(ContextAddress));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SystemContextMarkerAlive()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        return DiscardedMarker is { IsAlive: true } ? 1 : 0;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void DeferHoldingMarker(SSystemContext Context)
    {
        var Marker = new object();
        DiscardedMarker = new WeakReference(Marker);
        Context.Defer(_ => GC.KeepAlive(Marker));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SystemContextRaycast(IntPtr ContextAddress, int* OutEntity)
    {
        try
        {
            var Context = new SSystemContext(ContextAddress);
            SRayResult Hit = Context.Physics.Raycast(new FVector3(0.0f, 0.0f, -3.0f), new FVector3(0.0f, 0.0f, 3.0f));
            *OutEntity = (int)Hit.Entity;
            return Hit.bHit ? 1 : 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return -1;
        }
    }
}
