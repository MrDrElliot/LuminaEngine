using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Byte-backed on purpose, so a marshaller that assumed four bytes writes over the slot that follows.
internal enum ETestFrameChannel : byte
{
    Zero = 0,
    One = 1,
    Two = 2,
    Highest = 255,
}

// Stands in for a script type, so a native test can drive the real dispatcher over a minted frame.
internal sealed class FrameMarshalTarget
{
    [ScriptFunction]
    public void MarshalOut(int In, out int Out)
    {
        Out = In * 2;
    }

    [ScriptFunction]
    public void MarshalRef(ref int Value)
    {
        Value += 5;
    }

    // Named for the reflected function it stands in for, which is how the dispatcher resolves it.
    public float? OnEchoOptional(float? In)
    {
        return In.HasValue ? In.Value * 2.0f : null;
    }

    public float? OnMaybeDouble(float? In)
    {
        return In.HasValue ? In.Value + 0.5f : null;
    }

    // Reads the caller's vector in place and appends the total, so both directions of the view are observable.
    public void OnAppendSum(Lumina.TVector<float> Values)
    {
        float Sum = 0.0f;
        for (int Index = 0; Index < Values.Count; ++Index)
        {
            Sum += Values[Index];
        }

        Values.Add(Sum);
    }

    public void MarshalDirections(int Plain, out int Out, ref int Ref, in int In)
    {
        Out = Plain;
        Ref += In;
    }
}

// Kept off FrameMarshalTarget so the overload refusal cannot cost the dispatcher tests their own methods.
internal sealed class OverloadedFunctionTarget
{
    [ScriptFunction]
    public void Ambiguous(int Value)
    {
    }

    [ScriptFunction]
    public void Ambiguous(float Value)
    {
    }

    [ScriptFunction]
    public void Unambiguous()
    {
    }
}

// Drives FrameMarshal from a native test, which is the only way to exercise it without an editor.
internal static unsafe class InteropTestHooks
{
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr Test_MakeMarshalTarget()
    {
        return GCHandle.ToIntPtr(GCHandle.Alloc(new FrameMarshalTarget()));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_FreeMarshalTarget(IntPtr Handle)
    {
        if (Handle != IntPtr.Zero)
        {
            GCHandle.FromIntPtr(Handle).Free();
        }
    }

    // The packed direction of one parameter, pinning out, ref and in as the C# compiler actually emits them.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static uint Test_ParameterDirection(int Index)
    {
        var Method = typeof(FrameMarshalTarget).GetMethod(nameof(FrameMarshalTarget.MarshalDirections))!;
        ParameterInfo[] Signature = Method.GetParameters();
        return Index < 0 || Index >= Signature.Length
            ? uint.MaxValue
            : (uint)FrameMarshal.DirectionOf(Signature[Index]);
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameReadVector3(IntPtr Frame, IntPtr Property, float* OutXYZ)
    {
        if (!FrameMarshal.TryBind(Property, typeof(FVector3), "Test_FrameReadVector3", out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        FVector3 Value = (FVector3)FrameMarshal.Read(Frame, Slot)!;
        OutXYZ[0] = Value.X;
        OutXYZ[1] = Value.Y;
        OutXYZ[2] = Value.Z;
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameWriteVector3(IntPtr Frame, IntPtr Property, float X, float Y, float Z)
    {
        if (!FrameMarshal.TryBind(Property, typeof(FVector3), "Test_FrameWriteVector3", out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        FrameMarshal.Write(Frame, Slot, new FVector3(X, Y, Z));
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameReadByteEnum(IntPtr Frame, IntPtr Property)
    {
        if (!FrameMarshal.TryBind(Property, typeof(ETestFrameChannel), "Test_FrameReadByteEnum",
                out FrameMarshal.FSlot Slot))
        {
            return -1;
        }

        return (int)(ETestFrameChannel)FrameMarshal.Read(Frame, Slot)!;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameWriteByteEnum(IntPtr Frame, IntPtr Property, int Value)
    {
        if (!FrameMarshal.TryBind(Property, typeof(ETestFrameChannel), "Test_FrameWriteByteEnum",
                out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        FrameMarshal.Write(Frame, Slot, (ETestFrameChannel)Value);
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameReadEntity(IntPtr Frame, IntPtr Property, uint* OutId)
    {
        if (!FrameMarshal.TryBind(Property, typeof(Entity), "Test_FrameReadEntity", out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        *OutId = ((Entity)FrameMarshal.Read(Frame, Slot)!).Id;
        return 1;
    }

    // Returns 1 with the payload in OutValue, 0 for an unset optional, and -1 when the bind was refused.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameReadOptional(IntPtr Frame, IntPtr Property, float* OutValue)
    {
        if (!FrameMarshal.TryBind(Property, typeof(float?), "Test_FrameReadOptional",
                out FrameMarshal.FSlot Slot))
        {
            return -1;
        }

        object? Value = FrameMarshal.Read(Frame, Slot);
        if (Value == null)
        {
            return 0;
        }

        *OutValue = (float)Value;
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameWriteOptional(IntPtr Frame, IntPtr Property, int bSet, float Value)
    {
        if (!FrameMarshal.TryBind(Property, typeof(float?), "Test_FrameWriteOptional",
                out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        FrameMarshal.Write(Frame, Slot, bSet != 0 ? Value : null);
        return 1;
    }

    // Returns the element count seen through a bound view, or -1 when the bind was refused.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameReadVectorCount(IntPtr Frame, IntPtr Property, float* OutSum)
    {
        if (!FrameMarshal.TryBind(Property, typeof(Lumina.TVector<float>), "Test_FrameReadVectorCount",
                out FrameMarshal.FSlot Slot))
        {
            return -1;
        }

        var View = (Lumina.TVector<float>)FrameMarshal.Read(Frame, Slot)!;

        float Sum = 0.0f;
        foreach (float Value in View)
        {
            Sum += Value;
        }

        *OutSum = Sum;
        return View.Count;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameAppendToVector(IntPtr Frame, IntPtr Property, float Value)
    {
        if (!FrameMarshal.TryBind(Property, typeof(Lumina.TVector<float>), "Test_FrameAppendToVector",
                out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        ((Lumina.TVector<float>)FrameMarshal.Read(Frame, Slot)!).Add(Value);
        return 1;
    }

    // A vector slot must refuse a map view, since the two ops tables are not interchangeable.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameBindMapOverVector(IntPtr Property)
    {
        return FrameMarshal.TryBind(Property, typeof(Lumina.THashMap<int, int>), "Test_FrameBindMapOverVector",
            out FrameMarshal.FSlot _) ? 1 : 0;
    }

    // The kind TypeLibrary gives a nullable, so the schema half of a script-declared optional is pinned too.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ResolveNullableKind(int* OutPayloadKind)
    {
        var Library = new TypeLibrary(new[] { typeof(FrameMarshalTarget) });
        ScriptType Resolved = Library.ResolveType(typeof(float?), 0, new System.Collections.Generic.HashSet<Type>());

        *OutPayloadKind = Resolved.Element == null ? -1 : (int)Resolved.Element.Kind;
        return (int)Resolved.Kind;
    }

    // A width the frame cannot hold has to be refused rather than written, which is what this pins.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameBindMismatchedWidth(IntPtr Property)
    {
        return FrameMarshal.TryBind(Property, typeof(FVector3), "Test_FrameBindMismatchedWidth",
            out FrameMarshal.FSlot _) ? 1 : 0;
    }

    // Reads a reflected TVector out-param back through the generated array binding and reports what arrived.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_VectorBindingInts(int Count, int* OutFirst, int* OutLast, int* OutCalls)
    {
        CInteropTestLibrary.ResetMakeRangeCallCount();
        int[] Values = CInteropTestLibrary.MakeRange(Count);

        *OutCalls = CInteropTestLibrary.GetMakeRangeCallCount();
        *OutFirst = Values.Length > 0 ? Values[0] : -1;
        *OutLast = Values.Length > 0 ? Values[Values.Length - 1] : -1;
        return Values.Length;
    }

    // Reads a reflected string return back through the generated binding and reports what arrived.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_StringBinding(int Length, int* OutCalls, int* OutFirst, int* OutLast)
    {
        CInteropTestLibrary.ResetMakeNameCallCount();
        string Value = CInteropTestLibrary.MakeName(Length);

        *OutCalls = CInteropTestLibrary.GetMakeNameCallCount();
        *OutFirst = Value.Length > 0 ? Value[0] : -1;
        *OutLast = Value.Length > 0 ? Value[Value.Length - 1] : -1;
        return Value.Length;
    }

    // The entity element, which every converted buffer export uses, and a wider struct element beside it.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_VectorBindingEntities(int Count, uint* OutLast)
    {
        Entity[] Values = CInteropTestLibrary.MakeEntityRange(Count);
        *OutLast = Values.Length > 0 ? Values[Values.Length - 1].Id : 0;
        return Values.Length;
    }

    // An out container of objects crosses as raw pointers and is rebuilt through the wrapper cache, so every
    // element has to come back as the same managed instance rather than a fresh one per element.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ObjectArrayBinding(int Count, int* OutIdentical)
    {
        Lumina.CInteropTestLibrary[] Values = CInteropTestLibrary.MakeObjectRange(Count);

        bool Identical = true;
        for (int Index = 1; Index < Values.Length; ++Index)
        {
            Identical &= ReferenceEquals(Values[Index], Values[0]);
        }
        *OutIdentical = (Values.Length > 0 && Identical) ? 1 : 0;
        return Values.Length;
    }

    // A class handle resolved from the C# type has to name the same class on the native side.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ClassHandleRoundTrip()
    {
        Lumina.TSubclassOf<Lumina.CInteropTestLibrary> Class = CInteropTestLibrary.StaticClass();
        if (!Class.IsValid)
        {
            return 0;
        }
        return CInteropTestLibrary.ReadClassName(Class) == "CInteropTestLibrary" ? 1 : 0;
    }

    // A handle struct is how a non-reflectable native pointer crosses, so all 64 bits have to survive both ways.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static ulong Test_HandleStructRoundTrip(ulong Value)
    {
        Lumina.FUIElement Handle = CInteropTestLibrary.MakeHandle(Value);
        if (Handle.Handle != Value || Handle.IsValid != (Value != 0))
        {
            return 0;
        }
        return CInteropTestLibrary.ReadHandle(Handle);
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_VectorBindingStructs(int Count, float* OutLastY)
    {
        FVector3[] Values = CInteropTestLibrary.MakeVectorRange(Count);
        *OutLastY = Values.Length > 0 ? Values[Values.Length - 1].Y : 0.0f;
        return Values.Length;
    }

    // Binds a one-shot callback and hands its token back, so a native test can fire it and see the effect.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static ulong Test_BindScriptCallback()
    {
        CallbackPayload = 0xFFFFFFFFUL;
        CallbackRuns = 0;
        return ScriptCallback.Of((Entity Spawned) =>
        {
            CallbackPayload = Spawned.Id;
            ++CallbackRuns;
        }).Token;
    }

    // Binds a repeating callback over a float payload, the shape a tween value step uses.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static ulong Test_BindRepeatingScriptCallback()
    {
        CallbackPayload = 0xFFFFFFFFUL;
        CallbackRuns = 0;
        return ScriptCallback.OfRepeating((float Value) =>
        {
            CallbackPayload = (ulong)(long)(Value * 1000.0f);
            ++CallbackRuns;
        }).Token;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static ulong Test_ScriptCallbackResult(int* OutRuns)
    {
        *OutRuns = CallbackRuns;
        return CallbackPayload;
    }

    private static ulong CallbackPayload;
    private static int CallbackRuns;

    // Binds a wrapper to a CObject and keeps it, so a native test can kill the object and probe the wrapper.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr Test_BindObjectWrapper(IntPtr Object)
    {
        return GCHandle.ToIntPtr(GCHandle.Alloc(new Lumina.CWorld(Object)));
    }

    // 1 when the handle read through, 0 when IsValid said no, and -1 when reading it threw as it should.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ProbeObjectWrapper(IntPtr WrapperHandle, int* OutIsValid)
    {
        GCHandle Handle = GCHandle.FromIntPtr(WrapperHandle);
        var Wrapper = (Lumina.CWorld)Handle.Target!;
        *OutIsValid = Wrapper.IsValid ? 1 : 0;
        try
        {
            return Wrapper.Handle != IntPtr.Zero ? 1 : 0;
        }
        catch (InvalidOperationException)
        {
            return -1;
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_FreeObjectWrapper(IntPtr WrapperHandle)
    {
        GCHandle.FromIntPtr(WrapperHandle).Free();
    }

    // Whether the generator produced a typed invoker, so a test can tell it apart from the reflection fallback.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_HasGeneratedInvoker(int Which)
    {
        string Name = Which switch
        {
            0 => nameof(FrameMarshalTarget.MarshalOut),
            1 => nameof(FrameMarshalTarget.MarshalRef),
            _ => nameof(FrameMarshalTarget.OnAppendSum),
        };
        return ScriptInvokerRegistry.Find(typeof(FrameMarshalTarget), Name) != 0 ? 1 : 0;
    }

    // How many functions survive describing a type whose [ScriptFunction] names are not all distinct.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_DescribedFunctionCount(byte* OutAmbiguousDescribed)
    {
        var Library = new TypeLibrary(new[] { typeof(OverloadedFunctionTarget) });
        TypeDescription Description = new(typeof(OverloadedFunctionTarget));
        Description.Build(Library);

        byte Ambiguous = 0;
        foreach (ScriptFunction Function in Description.Functions)
        {
            if (Function.Name == nameof(OverloadedFunctionTarget.Ambiguous))
            {
                Ambiguous = 1;
            }
        }

        *OutAmbiguousDescribed = Ambiguous;
        return Description.Functions.Count;
    }
}
