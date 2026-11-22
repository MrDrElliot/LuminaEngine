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

internal struct CountMovers : IQueryFunc<Write<STransformComponent>, Read<SNameComponent>>
{
    public int Count;

    public void Invoke(FEntity Entity, Write<STransformComponent> Transform, Read<SNameComponent> Name) => ++Count;
}

// No script assembly runs in the tests, so the system is paired with its native object by hand the way a minted one would be.
#pragma warning disable LUM0112
internal sealed class FieldDeclaredTestSystem : CEntitySystem
#pragma warning restore LUM0112
{
    public TQuery<Write<STransformComponent>, Read<SNameComponent>, Without<SDisabledTag>> Movers = new();

    public TQuery<Read<SNameComponent>, With<SLifetimeComponent>> Expiring = new();

    public TEventReader<SCharacterJumpedEvent> Jumps = new();

    public TEventWriter<TestSystemPing> Pings = new();
}

#pragma warning disable LUM0112
internal sealed class MidWalkTestSystem : CEntitySystem
#pragma warning restore LUM0112
{
    public TQuery<Write<STransformComponent>, Read<SBuoyancyComponent>> Floaters = new();
}

internal static unsafe class SystemParamTestHooks
{
    // On its first row the walk drops the doomed rows' buoyancy, gives the joiners one, and grows the written pool by spawning transforms; every later row writes through its Write term.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_QueryMutatesMidWalk(IntPtr NativeSystem, IntPtr World, uint* Doomed, int DoomedCount, uint* Joiners, int JoinerCount, uint* Spawned, int SpawnCount,
                                               uint* OutFirst, double* OutSum)
    {
        var Tested = new MidWalkTestSystem();
        Tested.BindNativeHandle(NativeSystem);
        Tested.BindSystemParams();
        var Registry = new Lumina.CWorld(World);

        int Visited = 0;
        uint First = 0;
        double Sum = 0.0;
        Tested.Floaters.ForEach((FEntity Entity, Write<STransformComponent> Transform, Read<SBuoyancyComponent> Buoyancy) =>
        {
            Sum += Buoyancy.Value.Buoyancy;
            if (Visited == 0)
            {
                First = Entity.Id;
                for (int i = 0; i < DoomedCount; ++i)
                {
                    if (Doomed[i] != Entity.Id)
                    {
                        Registry.RemoveComponent<SBuoyancyComponent>(new FEntity(Doomed[i]));
                    }
                }
                for (int i = 0; i < JoinerCount; ++i)
                {
                    Registry.GetOrEmplaceComponent<SBuoyancyComponent>(new FEntity(Joiners[i])).Buoyancy = -1000.0f;
                }
                for (int i = 0; i < SpawnCount; ++i)
                {
                    Registry.GetOrEmplaceComponent<STransformComponent>(new FEntity(Spawned[i]));
                }
            }
            else
            {
                Transform.Value.LocalTransform.Location.X = 7.0f;
            }
            ++Visited;
        });

        *OutFirst = First;
        *OutSum = Sum;
        return Visited;
    }

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

        CountMovers Counter = default;
        Tested.Movers.ForEach(ref Counter);
        OutResults[10] = Counter.Count;
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_SystemParamsRelease(IntPtr Handle)
    {
        GCHandle.FromIntPtr(Handle).Free();
    }
}
