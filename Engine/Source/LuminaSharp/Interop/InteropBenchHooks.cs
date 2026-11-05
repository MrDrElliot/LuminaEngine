using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Timing loops pricing a crossing from the managed side, each returning nanoseconds per iteration.
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
            Total += CInteropTestLibrary.BenchMeasureString("InteropBenchmarkName");
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
            Total += CInteropTestLibrary.BenchGetString().Length;
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
        ref Lumina.STransformComponent View = ref Unsafe.AsRef<Lumina.STransformComponent>((void*)Component);
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
        var Opaque = new Lumina.SInteropOpaqueStruct(Storage);
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
        ref Lumina.STransformComponent View = ref Unsafe.AsRef<Lumina.STransformComponent>((void*)Component);
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

    // A typed view walked once per tick with a component touched per entity, the shape a CEntitySystem runs.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemViewWalk(nint World, int Ticks, int* OutEntities)
    {
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;

        int Seen = 0;
        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            Seen = 0;
            foreach (var Row in Registry.View<Lumina.STransformComponent>().Each())
            {
                Total += Row.First.LocalTransform.Location.X;
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
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;

        int Seen = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            Seen = 0;
            foreach (var Row in Registry.View<Lumina.STransformComponent>().Each())
            {
                Row.First.LocalTransform.Location.X += 1.0f;
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
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;

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

    // Two includes, so each row pays the per-include pool check twice.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemViewWalkTwo(nint World, int Ticks, int* OutEntities)
    {
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;

        int Seen = 0;
        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            Seen = 0;
            foreach (var Row in Registry.View<Lumina.STransformComponent, Lumina.SBuoyancyComponent>().Each())
            {
                Total += Row.First.LocalTransform.Location.X * Row.Second.Buoyancy;
                ++Seen;
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        *OutEntities = Seen;
        return Seen > 0 ? PerIterationNanos(Elapsed, Ticks * Seen) : 0.0;
    }

    // The native half of a view alone, so the snapshot and the chunk gather can be split from the managed cursor.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_ViewGatherOnly(nint World, int Ticks, int bBeginOnly)
    {
        IntPtr* Include = stackalloc IntPtr[1];
        Include[0] = ComponentOps<Lumina.STransformComponent>.Token;
        uint* Entities = stackalloc uint[1024];
        IntPtr* Pointers = stackalloc IntPtr[1024];

        int Seen = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            IntPtr State = Native.ViewBegin((ulong)World, Include, 1, null, 0);
            Seen = 0;
            if (bBeginOnly == 0)
            {
                int Count;
                while ((Count = Native.ViewNextChunk(State, Entities, Pointers, null, 1024, 1)) > 0)
                {
                    Seen += Count;
                }
            }
            Native.ViewEnd(State);
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        return PerIterationNanos(Elapsed, Ticks * 10000);
    }

    // Per-entity gets on a component small enough to stay in cache, so the interop cost is not hidden behind memory.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SmallGet(nint World, int Which, int Ticks, uint* Entities, int Count)
    {
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;

        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                FEntity Entity = new FEntity(Entities[Index]);
                switch (Which)
                {
                    case 0:
                        Total += Registry.GetComponent<Lumina.SBuoyancyComponent>(Entity).Buoyancy;
                        break;
                    case 1:
                        if (Registry.TryGetComponent<Lumina.SBuoyancyComponent>(Entity) is { } Found)
                        {
                            Total += Found.Get().Buoyancy;
                        }
                        break;
                    case 2:
                        Total += Registry.HasComponent<Lumina.SBuoyancyComponent>(Entity) ? 1.0f : 0.0f;
                        break;
                    default:
                        ref Lumina.SBuoyancyComponent Ref = ref Registry.GetComponent<Lumina.SBuoyancyComponent>(Entity);
                        Total += Ref.Buoyancy + Ref.LinearDrag + Ref.AngularDrag;
                        break;
                }
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Ticks * Count);
    }

    // A tracked reference kept across ticks, the way a script caches one in a field.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_KeptRefGet(nint World, int Ticks, uint* Entities, int Count)
    {
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;
        var Kept = new TComponentRef<Lumina.SBuoyancyComponent>[Count];
        for (int Index = 0; Index < Count; ++Index)
        {
            Kept[Index] = Registry.TryGetComponent<Lumina.SBuoyancyComponent>(new FEntity(Entities[Index])) ?? default;
        }

        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                Total += Kept[Index].Get().Buoyancy;
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Ticks * Count);
    }

    // One property operation a script performs on a native component, run Iterations times, with the managed bytes it allocated.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_PropertyOp(IntPtr Component, int Which, int Iterations, long* OutBytes)
    {
        ref Lumina.SInteropBenchComponent C = ref Unsafe.AsRef<Lumina.SInteropBenchComponent>((void*)Component);
        long Bytes = GC.GetAllocatedBytesForCurrentThread();
        double Nanos = Which switch
        {
            0 => RunOp<OpFloatsAdd>(ref C, Iterations),
            1 => RunOp<OpFloatsIndex>(ref C, Iterations),
            2 => RunOp<OpFloatsCount>(ref C, Iterations),
            3 => RunOp<OpPointsIndex>(ref C, Iterations),
            4 => RunOp<OpNamesGet>(ref C, Iterations),
            5 => RunOp<OpNamesSet>(ref C, Iterations),
            6 => RunOp<OpLabelRead>(ref C, Iterations),
            7 => RunOp<OpLabelWrite>(ref C, Iterations),
            8 => RunOp<OpTagAssign>(ref C, Iterations),
            9 => RunOp<OpTagCompare>(ref C, Iterations),
            10 => RunOp<OpScoresGet>(ref C, Iterations),
            11 => RunOp<OpScoresSet>(ref C, Iterations),
            12 => RunOp<OpInnerField>(ref C, Iterations),
            _ => RunOp<OpFloatsSpan>(ref C, Iterations),
        };
        *OutBytes = GC.GetAllocatedBytesForCurrentThread() - Bytes;
        return Nanos;
    }

    private interface IPropertyOp
    {
        static abstract float Run(ref Lumina.SInteropBenchComponent C, int Slot);
    }

    // Specialized per operation, so each loop compiles the way a script's own method would.
    private static double RunOp<TOp>(ref Lumina.SInteropBenchComponent C, int Iterations) where TOp : struct, IPropertyOp
    {
        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            Total += TOp.Run(ref C, i & 255);
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    private static readonly FName CachedTag = "BenchTag";

    private struct OpFloatsAdd : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { if (Slot == 0) { C.Floats.Clear(); } C.Floats.Add(1.0f); return 0.0f; } }
    private struct OpFloatsIndex : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { C.Floats[Slot] += 1.0f; return 0.0f; } }
    private struct OpFloatsCount : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) => C.Floats.Count; }
    private struct OpPointsIndex : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { C.Points[Slot].X += 1.0f; return 0.0f; } }
    private struct OpNamesGet : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) => C.Names.Get(Slot).ToString().Length; }
    private struct OpNamesSet : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { C.Names.Set(Slot, "abc"); return 0.0f; } }
    private struct OpLabelRead : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) => C.Label.Length; }
    private struct OpLabelWrite : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { C.Label = "hello"; return 0.0f; } }
    private struct OpTagAssign : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { C.Tag = "BenchTag"; return 0.0f; } }
    private struct OpTagCompare : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) => C.Tag == CachedTag ? 1.0f : 0.0f; }
    private struct OpScoresGet : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) => C.Scores[Slot]; }
    private struct OpScoresSet : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { C.Scores.Set(Slot, 1.0f); return 0.0f; } }
    private struct OpInnerField : IPropertyOp { public static float Run(ref Lumina.SInteropBenchComponent C, int Slot) { C.Inner.Value += 1.0f; return 0.0f; } }
    private struct OpFloatsSpan : IPropertyOp
    {
        public static float Run(ref Lumina.SInteropBenchComponent C, int Slot)
        {
            foreach (ref float Value in C.Floats.AsSpan()) { Value += 1.0f; }
            return 0.0f;
        }
    }

    // The pieces of a string property write and read, so the expensive one can be found.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_StringPieces(IntPtr Component, int Which, int Iterations)
    {
        ref Lumina.SInteropBenchComponent C = ref Unsafe.AsRef<Lumina.SInteropBenchComponent>((void*)Component);
        IntPtr FStringPtr = NativeBindings.FieldAt(Component, 48);
        byte* Scratch = stackalloc byte[256];
        int Total = 0;
        long Start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Iterations; ++i)
        {
            switch (Which)
            {
                case 0: Native.StringAssign(FStringPtr, "hello"); break;
                case 1: Interop.ToUtf8("hello", Scratch, 256, out int Length); Total += Length; break;
                case 2: Total += NativeMarshal.ReadString(FStringPtr).Length; break;
                case 3: Total += System.Text.Encoding.UTF8.GetString(Scratch, 26).Length; break;
                case 4: C.Label = "hello"; break;
                default: Total += C.Label.Length; break;
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }

    // An untracked native lookup per entity, the price of finding a row again from its entity and pool.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemLookupGet(nint World, int Ticks, uint* Entities, int Count)
    {
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;

        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                Total += Registry.GetComponent<Lumina.STransformComponent>(new FEntity(Entities[Index])).LocalTransform.Location.X;
            }
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = (int)Total;
        return PerIterationNanos(Elapsed, Ticks * Count);
    }

    // A random access per entity rather than a walk, which is what a system reaching a second component does.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_SystemRandomGet(nint World, int Ticks, uint* Entities, int Count)
    {
        var Registry = Wrapper<Lumina.CWorld>.ForObject(World)!;

        float Total = 0.0f;
        long Start = Stopwatch.GetTimestamp();
        for (int Tick = 0; Tick < Ticks; ++Tick)
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                if (Registry.TryGetComponent<Lumina.STransformComponent>(new FEntity(Entities[Index])) is { } Transform)
                {
                    Total += Transform.Get().LocalTransform.Location.X;
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

    // NativeCall against P/Invoke on the same exports, so each pair of rows differs only in the managed calling mechanism.
    private const string BenchLibrary = "Runtime";
    private const string ShortText = "InteropBenchmarkName";
    private static readonly string LongText = new('x', 400);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_InteropBench_NoOp")]
    private static partial int NativeCallNoOp();

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_InteropBench_NoOp", SuppressGCTransition = true)]
    private static partial int NativeCallNoOpSuppressed();

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_InteropBench_AddVectors")]
    private static partial FVector3 NativeCallAddVectors(FVector3 A, FVector3 B);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_InteropBench_CountedUtf8")]
    private static partial int NativeCallUtf8(string Text);

    [DllImport(BenchLibrary, EntryPoint = "LuminaSharp_InteropBench_NoOp")]
    private static extern int DllImportNoOp();

    [LibraryImport(BenchLibrary, EntryPoint = "LuminaSharp_InteropBench_NoOp")]
    private static partial int LibraryImportNoOp();

    [LibraryImport(BenchLibrary, EntryPoint = "LuminaSharp_InteropBench_NoOp")]
    [SuppressGCTransition]
    private static partial int LibraryImportNoOpSuppressed();

    [LibraryImport(BenchLibrary, EntryPoint = "LuminaSharp_InteropBench_AddVectors")]
    private static partial FVector3 LibraryImportAddVectors(FVector3 A, FVector3 B);

    [LibraryImport(BenchLibrary, EntryPoint = "LuminaSharp_InteropBench_TerminatedUtf8", StringMarshalling = StringMarshalling.Utf8)]
    private static partial int LibraryImportUtf8(string Text);

    [LibraryImport(BenchLibrary, EntryPoint = "LuminaSharp_InteropBench_TerminatedUtf16", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int LibraryImportUtf16(string Text);

    [LibraryImport(BenchLibrary, EntryPoint = "LuminaSharp_InteropBench_StaticUtf8")]
    private static partial byte* LibraryImportStaticUtf8();

    // The bare function pointer a NativeCall wraps, to split the cost of the pointer call from the wrapper around it.
    private static readonly delegate* unmanaged[Cdecl]<int> RawNoOp = (delegate* unmanaged[Cdecl]<int>)NativeBindings.ResolveFrom(Host.ModuleHandle("Runtime"), "LuminaSharp_InteropBench_NoOp");
    private static readonly delegate* unmanaged[Cdecl, SuppressGCTransition]<int> RawNoOpSuppressed = (delegate* unmanaged[Cdecl, SuppressGCTransition]<int>)RawNoOp;
    private static readonly delegate* unmanaged[Cdecl]<FVector3, FVector3, FVector3> RawAddVectors = (delegate* unmanaged[Cdecl]<FVector3, FVector3, FVector3>)NativeBindings.ResolveFrom(Host.ModuleHandle("Runtime"), "LuminaSharp_InteropBench_AddVectors");

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static double Bench_InteropCompare(int Which, int Iterations)
    {
        int Total = 0;
        FVector3 One = new FVector3(1.0f, 2.0f, 3.0f);
        long Start = Stopwatch.GetTimestamp();
        switch (Which)
        {
            case 0: for (int i = 0; i < Iterations; ++i) { Total += NativeCallNoOp(); } break;
            case 1: for (int i = 0; i < Iterations; ++i) { Total += NativeCallNoOpSuppressed(); } break;
            case 2: for (int i = 0; i < Iterations; ++i) { Total += DllImportNoOp(); } break;
            case 3: for (int i = 0; i < Iterations; ++i) { Total += LibraryImportNoOp(); } break;
            case 4: for (int i = 0; i < Iterations; ++i) { Total += LibraryImportNoOpSuppressed(); } break;
            case 5: for (int i = 0; i < Iterations; ++i) { Total += (int)NativeCallAddVectors(One, One).X; } break;
            case 6: for (int i = 0; i < Iterations; ++i) { Total += (int)LibraryImportAddVectors(One, One).X; } break;
            case 7: for (int i = 0; i < Iterations; ++i) { Total += NativeCallUtf8(ShortText); } break;
            case 8: for (int i = 0; i < Iterations; ++i) { Total += LibraryImportUtf8(ShortText); } break;
            case 9: for (int i = 0; i < Iterations; ++i) { Total += LibraryImportUtf16(ShortText); } break;
            case 10: for (int i = 0; i < Iterations; ++i) { Total += NativeCallUtf8(LongText); } break;
            case 11: for (int i = 0; i < Iterations; ++i) { Total += LibraryImportUtf8(LongText); } break;
            case 12: for (int i = 0; i < Iterations; ++i) { Total += LibraryImportUtf16(LongText); } break;
            case 13: for (int i = 0; i < Iterations; ++i) { Total += CInteropTestLibrary.BenchGetString().Length; } break;
            case 14: for (int i = 0; i < Iterations; ++i) { Total += Marshal.PtrToStringUTF8((IntPtr)LibraryImportStaticUtf8())!.Length; } break;
            case 15: for (int i = 0; i < Iterations; ++i) { Total += RawNoOp(); } break;
            case 16: for (int i = 0; i < Iterations; ++i) { Total += RawNoOpSuppressed(); } break;
            case 17: for (int i = 0; i < Iterations; ++i) { Total += (int)RawAddVectors(One, One).X; } break;
        }
        long Elapsed = Stopwatch.GetTimestamp() - Start;
        Sink = Total;
        return PerIterationNanos(Elapsed, Iterations);
    }
}
