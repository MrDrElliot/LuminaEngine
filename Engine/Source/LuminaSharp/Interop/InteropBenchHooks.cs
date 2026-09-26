using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Managed-side timing loops, so a benchmark can price a managed to native crossing from the side that pays
// for the marshalling. Each entry returns nanoseconds per iteration.
internal static unsafe partial class InteropBenchHooks
{
    private static volatile int Sink;

    private static double PerIterationNanos(long Ticks, int Iterations)
    {
        double Seconds = (double)Ticks / Stopwatch.Frequency;
        return Iterations > 0 ? (Seconds * 1e9) / Iterations : 0.0;
    }

    // The floor for a managed to native call, an int return and no arguments.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_NativeCallNoArgs(int Iterations)
    {
        int Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += CInteropTestLibrary.BenchNoOp();
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // The same call with the GC transition skipped, which is the whole difference between the two.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_NativeCallSuppressed(int Iterations)
    {
        int Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += BenchNoOpFast();
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_NativeCallBlittableStruct(int Iterations)
    {
        FVector3 Accumulated = default;
        FVector3 One = new FVector3(1.0f, 2.0f, 3.0f);
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Accumulated = CInteropTestLibrary.BenchAddVectors(Accumulated, One);
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Accumulated.X;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // A string argument, which encodes UTF-8 into stack scratch on every call.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_NativeCallStringArg(int Iterations)
    {
        int Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += CInteropTestLibrary.BenchMeasureName("InteropBenchmarkName");
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // A string return, which is the two-pass protocol plus a managed string allocation.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_NativeCallStringReturn(int Iterations)
    {
        int Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += CInteropTestLibrary.BenchGetName().Length;
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // An out container, which fits the stack scratch at this size and so costs one crossing plus one array.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_NativeCallArrayReturn(int Iterations, int Elements)
    {
        int Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += CInteropTestLibrary.MakeRange(Elements).Length;
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // Pure managed work at the same shape, the floor any crossing is measured against.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_ManagedOnly(int Iterations)
    {
        int Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += ManagedNoOp();
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int ManagedNoOp() => 0;

    // Native does the crossing for this one; the body only has to be cheap enough not to hide it.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Bench_ManagedEntryPoint(int Value)
    {
        return Value + 1;
    }

    // An offset load straight into native memory, which crosses nothing at all.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_PropertyOffsetRead(IntPtr Component, int Iterations)
    {
        var View = new Lumina.STransformComponent(Component);
        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += View.LocalTransform.Location.X;
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // A map whose key and value are plain values, so the marshalled branch has to fold away entirely.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_BlittableMapLookup(IntPtr Storage, int Iterations)
    {
        var Opaque = new Lumina.FInteropOpaqueStruct(Storage);
        Opaque.Scores.Set(1, 1.0f);

        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Opaque.Scores.TryGetValue(1, out float Value);
            Total += Value;
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // The same value through a reflected accessor, which is a real crossing even with the transition off.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_PropertyAccessorRead(IntPtr Component, int Iterations)
    {
        var View = new Lumina.STransformComponent(Component);
        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += View.GetLocalLocation().X;
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // Every generated CObject accessor reads through Handle, so this is the tax on all of them.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_CObjectHandleResolve(IntPtr Object, int Iterations)
    {
        var Wrapper = new Lumina.CWorld(Object);
        long Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += (long)Wrapper.Handle;
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // The shape a CEntitySystem actually runs, a typed view walked once per tick with a component touched
    // per entity. This is the number that decides whether a system belongs in C# at all.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemViewWalk(nint World, int Ticks, int* OutEntities)
    {
        var Registry = new EntityRegistry((ulong)World);

        int Seen = 0;
        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            Seen = 0;
            foreach ((Entity _, Lumina.STransformComponent Transform) in Registry.View<Lumina.STransformComponent>())
            {
                Total += Transform.LocalTransform.Location.X;
                ++Seen;
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        *OutEntities = Seen;
        return Seen > 0 ? PerIterationNanos(Elapsed, Ticks * Seen) : 0.0;
    }

    // The same walk writing the component back, since a system that only reads is the easy half.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemViewWrite(nint World, int Ticks, int* OutEntities)
    {
        var Registry = new EntityRegistry((ulong)World);

        int Seen = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            Seen = 0;
            foreach ((Entity _, Lumina.STransformComponent Transform) in Registry.View<Lumina.STransformComponent>())
            {
                FTransform Local = Transform.LocalTransform;
                Local.Location.X += 1.0f;
                Transform.LocalTransform = Local;
                ++Seen;
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        *OutEntities = Seen;
        return Seen > 0 ? PerIterationNanos(Elapsed, Ticks * Seen) : 0.0;
    }

    // MoveNext alone, so the walk's cost can be split from what reading Current does.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemViewStepOnly(nint World, int Ticks, int* OutEntities)
    {
        var Registry = new EntityRegistry((ulong)World);

        int Seen = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            Seen = 0;
            var Step = Registry.View<Lumina.STransformComponent>().GetEnumerator();
            while (Step.MoveNext())
            {
                ++Seen;
            }
            Step.Dispose();
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        *OutEntities = Seen;
        return Seen > 0 ? PerIterationNanos(Elapsed, Ticks * Seen) : 0.0;
    }

    // A random access per entity rather than a walk, which is what a system reaching a second component does.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemRandomGet(nint World, int Ticks, uint* Entities, int Count)
    {
        var Registry = new EntityRegistry((ulong)World);

        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                Lumina.STransformComponent? Transform =
                    Registry.TryGet<Lumina.STransformComponent>(new Entity(Entities[Index]));
                if (Transform != null)
                {
                    Total += Transform.LocalTransform.Location.X;
                }
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Ticks * Count);
    }

    [NativeCall(Module = "Runtime",
        EntryPoint = "LuminaSharp_Call_Lumina_CInteropTestLibrary_BenchNoOp", SuppressGCTransition = true)]
    private static partial int BenchNoOpFast();
}
