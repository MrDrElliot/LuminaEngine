using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

#pragma warning disable LUM0112
internal sealed class QueryBenchSystem : CEntitySystem
#pragma warning restore LUM0112
{
    public TQuery<Write<STransformComponent>> One = new();
    public TQuery<Write<STransformComponent>, Read<SBuoyancyComponent>> Two = new();
    public TQuery<Write<STransformComponent>, Read<SBuoyancyComponent>, Read<SLifetimeComponent>> Three = new();
    public TQuery<Write<STransformComponent>, Read<SLifetimeComponent>, Without<SDisabledTag>> TwoWithout = new();
}

internal struct MoveOne : IQueryFunc<Write<STransformComponent>>
{
    public void Invoke(FEntity Entity, Write<STransformComponent> Transform) => Transform.Value.LocalTransform.Location.X += 1.0f;
}

internal struct MoveByBuoyancy : IQueryFunc<Write<STransformComponent>, Read<SBuoyancyComponent>>
{
    public void Invoke(FEntity Entity, Write<STransformComponent> Transform, Read<SBuoyancyComponent> Buoyancy) => Transform.Value.LocalTransform.Location.X += Buoyancy.Value.Buoyancy;
}

internal struct MoveByBoth : IQueryFunc<Write<STransformComponent>, Read<SBuoyancyComponent>, Read<SLifetimeComponent>>
{
    public void Invoke(FEntity Entity, Write<STransformComponent> Transform, Read<SBuoyancyComponent> Buoyancy, Read<SLifetimeComponent> Lifetime)
        => Transform.Value.LocalTransform.Location.X += Buoyancy.Value.Buoyancy * Lifetime.Value.Lifetime;
}

internal struct MoveByLifetime : IQueryFunc<Write<STransformComponent>, Read<SLifetimeComponent>>
{
    public void Invoke(FEntity Entity, Write<STransformComponent> Transform, Read<SLifetimeComponent> Lifetime) => Transform.Value.LocalTransform.Location.X += Lifetime.Value.Lifetime;
}

// The C# half of FInteropBench.QueryForEachScaling, each case the same per-entity write its C++ twin does, cases 4 to 7 through a struct callback.
internal static unsafe class QueryBenchHooks
{
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr Bench_QueryBind(IntPtr NativeSystem)
    {
        var Bench = new QueryBenchSystem();
        Bench.BindNativeHandle(NativeSystem);
        Bench.BindSystemParams();
        return GCHandle.ToIntPtr(GCHandle.Alloc(Bench));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_QueryForEach(IntPtr Handle, int Which, int Ticks, int Entities)
    {
        if (GCHandle.FromIntPtr(Handle).Target is not QueryBenchSystem Bench)
        {
            return -1.0;
        }

        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            switch (Which)
            {
                case 0:
                    Bench.One.ForEach(static (FEntity Entity, Write<STransformComponent> Transform) => Transform.Value.LocalTransform.Location.X += 1.0f);
                    break;
                case 1:
                    Bench.Two.ForEach(static (FEntity Entity, Write<STransformComponent> Transform, Read<SBuoyancyComponent> Buoyancy) =>
                        Transform.Value.LocalTransform.Location.X += Buoyancy.Value.Buoyancy);
                    break;
                case 2:
                    Bench.Three.ForEach(static (FEntity Entity, Write<STransformComponent> Transform, Read<SBuoyancyComponent> Buoyancy, Read<SLifetimeComponent> Lifetime) =>
                        Transform.Value.LocalTransform.Location.X += Buoyancy.Value.Buoyancy * Lifetime.Value.Lifetime);
                    break;
                case 4:
                {
                    MoveOne Func = default;
                    Bench.One.ForEach(ref Func);
                    break;
                }
                case 5:
                {
                    MoveByBuoyancy Func = default;
                    Bench.Two.ForEach(ref Func);
                    break;
                }
                case 6:
                {
                    MoveByBoth Func = default;
                    Bench.Three.ForEach(ref Func);
                    break;
                }
                case 7:
                {
                    MoveByLifetime Func = default;
                    Bench.TwoWithout.ForEach(ref Func);
                    break;
                }
                default:
                    Bench.TwoWithout.ForEach(static (FEntity Entity, Write<STransformComponent> Transform, Read<SLifetimeComponent> Lifetime) =>
                        Transform.Value.LocalTransform.Location.X += Lifetime.Value.Lifetime);
                    break;
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        return Elapsed * 1e9 / Stopwatch.Frequency / ((double)Ticks * Entities);
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Bench_QueryRelease(IntPtr Handle)
    {
        GCHandle.FromIntPtr(Handle).Free();
    }
}
