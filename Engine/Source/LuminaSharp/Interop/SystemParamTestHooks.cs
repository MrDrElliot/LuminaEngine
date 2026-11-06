using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Lumina;

namespace LuminaSharp;

internal struct TestSystemPing : IEvent
{
    public int Value;
}

// No script assembly runs in the tests, so the system is paired with its native object by hand the way a minted one would be.
#pragma warning disable LUM0112
internal sealed class FieldDeclaredTestSystem : CEntitySystem
#pragma warning restore LUM0112
{
    [Without<SDisabledTag>]
    public TQuery<Write<STransformComponent>, Read<SNameComponent>> Movers = new();

    [With<SLifetimeComponent>]
    public TQuery<Read<SNameComponent>> Expiring = new();

    public TEventReader<SCharacterJumpedEvent> Jumps = new();

    public TEventWriter<TestSystemPing> Pings = new();
}

internal static unsafe class SystemParamTestHooks
{
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr Test_SystemParamsBind(IntPtr NativeSystem)
    {
        var Tested = new FieldDeclaredTestSystem();
        Tested.BindNativeHandle(NativeSystem);
        Tested.BindSystemParams();
        return GCHandle.ToIntPtr(GCHandle.Alloc(Tested));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SystemParamsRun(IntPtr Handle, uint Probe, uint Excluded, int* OutResults)
    {
        if (GCHandle.FromIntPtr(Handle).Target is not FieldDeclaredTestSystem Tested)
        {
            return 0;
        }

        int Serial = 0;
        Tested.Movers.ForEach((FEntity Entity, Write<STransformComponent> Transform, Read<SNameComponent> Name) =>
        {
            Transform.Value.LocalTransform.Location.X += 1.0f;
            ++Serial;
        });

        int Parallel = 0;
        Tested.Movers.ParallelForEach((FEntity Entity, Write<STransformComponent> Transform, Read<SNameComponent> Name) => Interlocked.Increment(ref Parallel), 1);

        int JumpSum = 0;
        foreach (ref readonly SCharacterJumpedEvent Jump in Tested.Jumps.Read())
        {
            JumpSum += Jump.JumpCount;
        }

        int RejectedWrite = 0;
        try
        {
            Tested.Movers.TryGet(new FEntity(Probe), out Write<SNameComponent> _);
        }
        catch (InvalidOperationException)
        {
            RejectedWrite = 1;
        }

        Tested.Pings.Send(new TestSystemPing { Value = 7 });

        OutResults[0] = Serial;
        OutResults[1] = Parallel;
        OutResults[2] = JumpSum;
        OutResults[3] = Tested.Jumps.HasUnread() ? 1 : 0;
        OutResults[4] = Tested.Movers.TryGet(new FEntity(Probe), out Read<SNameComponent> _) ? 1 : 0;
        OutResults[5] = Tested.Movers.Contains(new FEntity(Excluded)) ? 1 : 0;
        OutResults[6] = RejectedWrite;
        OutResults[7] = Tested.Movers.IsEmpty ? 1 : 0;

        int WithSeen = 0;
        Tested.Expiring.ForEach((FEntity Entity, Read<SNameComponent> Name) => WithSeen += Entity.Id == Probe ? 1 : 100);
        OutResults[8] = WithSeen;
        OutResults[9] = Tested.Expiring.Contains(new FEntity(Excluded)) ? 1 : 0;
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_SystemParamsRelease(IntPtr Handle)
    {
        GCHandle.FromIntPtr(Handle).Free();
    }
}
