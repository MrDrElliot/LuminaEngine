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

    // A string parameter, which used to have no generated invoker and fell to the boxing reflection path.
    [ScriptFunction]
    public void MarshalText(string In, out int Length)
    {
        Length = In == null ? -1 : In.Length;
    }

    // An opaque struct wrapper, which stays on the reflection path and is read through a view over the slot.
    [ScriptFunction]
    public void MarshalStructView(Lumina.FAnimGraphBoneMaskBone Bone, out float Weight)
    {
        Weight = Bone == null ? -1.0f : Bone.Weight;
    }

    // An async body that yields once, so the token is observably running before the tick completes it.
    [ScriptFunction]
    public async System.Threading.Tasks.Task MarshalAsync(int Value)
    {
        AsyncGate = new System.Threading.Tasks.TaskCompletionSource();
        await AsyncGate.Task;
        AsyncSeen = Value;
    }

    internal static System.Threading.Tasks.TaskCompletionSource? AsyncGate;
    internal static int AsyncSeen;

    // A delegate and an instanced struct name the slot they view, which the frame binder used to refuse.
    [ScriptFunction]
    public void MarshalSlotViews(ScriptDelegate Event, Lumina.FInstancedStruct Instanced, out int Seen)
    {
        Seen = (Event.IsValid ? 1 : 0) | (Instanced.IsBound ? 2 : 0);
    }

    // A container by value, which the frame can hand back because it owns the slot the copy fills.
    public System.Collections.Generic.List<int> OnBuildRange(int Count)
    {
        var Built = new System.Collections.Generic.List<int>();
        for (int Index = 0; Index < Count; ++Index)
        {
            Built.Add(Index * 3);
        }
        return Built;
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
            2 => nameof(FrameMarshalTarget.MarshalText),
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

    // Reads and rewrites a bare native FSoftObjectPath through the element marshaller, the container shape.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SoftPathElementRoundTrip(IntPtr Slot, int* OutWrittenLength)
    {
        Lumina.FSoftObjectPath Read = ElementMarshal.Read<Lumina.FSoftObjectPath>(Slot);

        ElementMarshal.Write(Slot, new Lumina.FSoftObjectPath("/Game/Written"));
        *OutWrittenLength = Native.SoftPathGet(Slot).Length;

        return Read.Path != null ? Read.Path.Length : 0;
    }

    // The managed-string and bool slots the frame and property lanes need, through the one marshaller.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SlotMarshalRoundTrip(IntPtr StringSlot, IntPtr BoolSlot, int* OutBoolRead,
        int* OutBoolIsMarshalled)
    {
        string Read = ElementMarshal.Read<string>(StringSlot);
        ElementMarshal.Write(StringSlot, "written");

        *OutBoolRead = ElementMarshal.Read<bool>(BoolSlot) ? 1 : 0;
        ElementMarshal.Write(BoolSlot, false);
        *OutBoolIsMarshalled = ElementKind<bool>.IsMarshalled ? 1 : 0;

        return Read.Length;
    }

    // The same optional slot the frame lane binds, reached by address and token instead of container and property.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_OptionalSlotRoundTrip(IntPtr Slot, IntPtr Property, float Written, float* OutRead)
    {
        float? Read = ElementMarshal.Read<float?>(Slot, Property);

        ElementMarshal.Write<float?>(Slot, Property, Written);
        float? Back = ElementMarshal.Read<float?>(Slot, Property);
        *OutRead = Back ?? -1.0f;

        ElementMarshal.Write<float?>(Slot, Property, null);
        bool Cleared = !ElementMarshal.Read<float?>(Slot, Property).HasValue;

        return (Read.HasValue ? 1 : 0) | (Cleared ? 2 : 0);
    }

    // A compiled factory closes over a user type, so a Reset that misses its cache pins the unloading generation.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_FrameMarshalCachesClear(IntPtr StructProperty, IntPtr VectorProperty, int* OutBefore)
    {
        FrameMarshal.Reset();

        FrameMarshal.TryBind(StructProperty, typeof(Lumina.FAnimGraphBoneMaskBone), "CacheProbe", out _);
        FrameMarshal.TryBind(VectorProperty, typeof(Lumina.TVector<float>), "CacheProbe", out _);

        *OutBefore = FrameMarshal.CachedTypeCount;
        FrameMarshal.Reset();
        return FrameMarshal.CachedTypeCount;
    }

    // One classifier answers for both the container element cache and the call-frame binder, so pin what it says.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ElementKindsAgree()
    {
        (Type Slot, EElementKind Expected)[] Cases =
        {
            (typeof(int), EElementKind.Blittable),
            (typeof(FVector3), EElementKind.Blittable),
            (typeof(bool), EElementKind.Bool),
            (typeof(string), EElementKind.ManagedString),
            (typeof(Lumina.FString), EElementKind.String),
            (typeof(float?), EElementKind.Optional),
            (typeof(Lumina.TVector<float>), EElementKind.Vector),
            (typeof(Lumina.THashMap<int, float>), EElementKind.Map),
            (typeof(Lumina.FSoftObjectPath), EElementKind.SoftRef),
            (typeof(Lumina.FAnimGraphBoneMaskBone), EElementKind.StructView),
            (typeof(Lumina.CWorld), EElementKind.ObjectWrapper),
            (typeof(Lumina.TObjectPtr<Lumina.CWorld>), EElementKind.ObjectRef),

            // A class handle is its own value, so copying the pointer is the whole of it.
            (typeof(Lumina.TSubclassOf<Lumina.CWorld>), EElementKind.Blittable),
            (typeof(Lumina.TSubStructOf<Lumina.FInstancedStruct>), EElementKind.Blittable),

            // Views holding a slot address, bound in place rather than copied over.
            (typeof(Lumina.FInstancedStruct), EElementKind.SlotView),
            (typeof(ScriptDelegate), EElementKind.SlotView),

            // A copy a frame can hand back, and the view spelling for a payload Nullable cannot carry.
            (typeof(System.Collections.Generic.List<int>), EElementKind.VectorCopy),
            (typeof(int[]), EElementKind.VectorCopy),
            (typeof(Lumina.TOptional<string>), EElementKind.OptionalView),
            (typeof(ScriptDelegate<float>), EElementKind.SlotView),
        };

        for (int Index = 0; Index < Cases.Length; ++Index)
        {
            if (ElementKinds.Of(Cases[Index].Slot) != Cases[Index].Expected)
            {
                return Index;
            }
        }

        return -1;
    }

    // A marshalled key and a marshalled value, both of which the map view used to refuse outright.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_MarshalledMapRoundTrip(IntPtr Storage)
    {
        var Opaque = new Lumina.FInteropOpaqueStruct(Storage);

        Opaque.Weights.Set("alpha", 1.5f);
        Opaque.Weights.Set("beta", 2.5f);
        Opaque.Labels.Set(7, "seven");

        int Result = 0;
        if (Opaque.Weights.Count == 2)
        {
            Result |= 1;
        }
        if (Opaque.Weights.TryGetValue("beta", out float Weight) && Weight == 2.5f)
        {
            Result |= 2;
        }
        if (Opaque.Labels.TryGetValue(7, out string Label) && Label == "seven")
        {
            Result |= 4;
        }
        if (Opaque.Weights.Remove("alpha") && Opaque.Weights.Count == 1)
        {
            Result |= 8;
        }
        if (!Opaque.Weights.ContainsKey("alpha"))
        {
            Result |= 16;
        }

        foreach (System.Collections.Generic.KeyValuePair<string, float> Pair in Opaque.Weights)
        {
            if (Pair.Key == "beta" && Pair.Value == 2.5f)
            {
                Result |= 32;
            }
        }

        return Result;
    }

    // A payload a Nullable cannot spell, which the optional binder used to refuse outright.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_MarshalledOptionalRoundTrip(IntPtr Storage)
    {
        var Opaque = new Lumina.FInteropOpaqueStruct(Storage);

        int Result = Opaque.Note.HasValue ? 0 : 1;

        Opaque.Note.Set("engaged");
        if (Opaque.Note.HasValue && Opaque.Note.Value == "engaged")
        {
            Result |= 2;
        }
        if (Opaque.Note.TryGetValue(out string Stored) && Stored == "engaged")
        {
            Result |= 4;
        }

        Opaque.Note.Reset();
        if (!Opaque.Note.HasValue)
        {
            Result |= 8;
        }

        return Result;
    }

    // A continuation posted from a worker must wait for the tick rather than running on that worker.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ContinuationResumesOnTheGameThread(int* OutRanBeforeTick, int* OutOnGameThread)
    {
        int GameThreadId = System.Environment.CurrentManagedThreadId;
        int Ran = 0;
        int ResumedOn = 0;

        System.Threading.SynchronizationContext Context = System.Threading.SynchronizationContext.Current!;
        if (Context == null)
        {
            return 0;
        }

        var Worker = new System.Threading.Thread(() =>
            Context.Post(_ =>
            {
                Ran = 1;
                ResumedOn = System.Environment.CurrentManagedThreadId;
            }, null));

        Worker.Start();
        Worker.Join();

        *OutRanBeforeTick = Ran;
        GameThreadContext.Drain();
        *OutOnGameThread = ResumedOn == GameThreadId ? 1 : 0;

        return Ran;
    }

    // A pending await has to be completed by an unload, or its await site never resumes and never unwinds.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_PendingAwaitIsCanceledOnUnload(int* OutTrackedBefore)
    {
        var Source = new FTestPending();
        *OutTrackedBefore = GameTaskRegistry.PendingCount;

        GameTaskRegistry.CancelAll();

        return (Source.Canceled ? 1 : 0) | (GameTaskRegistry.PendingCount == 0 ? 2 : 0);
    }

    private sealed class FTestPending : GameTaskRegistry.ICancelPending
    {
        internal bool Canceled;

        internal FTestPending()
        {
            GameTaskRegistry.Track(this);
        }

        public void Cancel()
        {
            Canceled = true;
        }
    }

    // Native gets a token for an async script function, polls it, and sees it finish on the tick.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_AsyncScriptFunctionState(ulong Token, int bRelease)
    {
        if (bRelease != 0)
        {
            ScriptAsync.Release(Token);
            return 0;
        }
        return (int)ScriptAsync.StateOf(Token);
    }

    // The reaper is what retires a fire-and-forget token, since native never asks about one again.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_AsyncRunningCount() => ScriptAsync.RunningCount;

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_ReapCompletedAsync() => ScriptAsync.ReapCompleted();

    // Completes the gate the async body is parked on, then drains so its continuation runs.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ReleaseAsyncGate()
    {
        FrameMarshalTarget.AsyncGate?.TrySetResult();
        GameThreadContext.Drain();
        return FrameMarshalTarget.AsyncSeen;
    }

    // Returns 0 when the bind was refused, 1 when the view came back unbound, and 2 when it names its slot.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SlotViewBinds(IntPtr Property, IntPtr Container, int bDelegate)
    {
        Type Declared = bDelegate != 0 ? typeof(ScriptDelegate) : typeof(Lumina.FInstancedStruct);
        if (!FrameMarshal.TryBind(Property, Declared, "SlotViewProbe", out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        object? View = FrameMarshal.Read(Container, Slot);
        bool bNamesItsSlot = View is Lumina.FInstancedStruct Instanced
            ? Instanced.IsBound
            : View is ScriptDelegate Delegate && Delegate.IsValid;

        return bNamesItsSlot ? 2 : 1;
    }

    // An opaque struct argument crosses as the address its wrapper views, which the binder used to refuse.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static float Test_OpaqueStructArgument(IntPtr Storage)
    {
        Lumina.FInteropOpaqueStruct Opaque = Storage == IntPtr.Zero
            ? null!
            : new Lumina.FInteropOpaqueStruct(Storage);

        return CInteropTestLibrary.ReadOpaqueValue(Opaque);
    }

    private static readonly DelegateBinding[] DelegateProbeBindings = new DelegateBinding[3];
    private static int DelegateProbeFlags;

    // Binds every arity the generated accessors expose, over a native SDelegateTestHost the caller owns.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_DelegateArgsBind(IntPtr Host)
    {
        DelegateProbeFlags = 0;

        Lumina.SDelegateTestHost View = new Lumina.SDelegateTestHost(Host);

        DelegateProbeBindings[0] = View.OnOneString.Bind(Text =>
        {
            DelegateProbeFlags |= 1;
            if (Text == "solo") DelegateProbeFlags |= 2;
        });

        DelegateProbeBindings[1] = View.OnTwo.Bind((Payload, Scalar) =>
        {
            DelegateProbeFlags |= 4;
            if (Payload.X == 2.0f && Payload.Count == 5 && Scalar == 0.25f) DelegateProbeFlags |= 8;
        });

        DelegateProbeBindings[2] = View.OnThree.Bind((Payload, Scalar, Text) =>
        {
            DelegateProbeFlags |= 16;
            if (Payload.X == 7.0f && Payload.Count == 3 && Scalar == 1.5f && Text == "hello") DelegateProbeFlags |= 32;
        });

        foreach (DelegateBinding Binding in DelegateProbeBindings)
        {
            if (!Binding.IsValid)
            {
                return 0;
            }
        }
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_DelegateArgsFlags() => DelegateProbeFlags;

    // Must run while the native delegates are still alive, or the bindings outlive the memory they name.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_DelegateArgsRelease()
    {
        for (int Index = 0; Index < DelegateProbeBindings.Length; ++Index)
        {
            DelegateProbeBindings[Index].Dispose();
            DelegateProbeBindings[Index] = default;
        }
    }

    // A blittable argument is read in place, so a two-argument broadcast must not allocate at all.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static long Test_DelegateArgsAllocated() => GC.GetAllocatedBytesForCurrentThread();

    // PropertyOffset returns an int32 and takes one pointer, so twelve is its width and anything else is drift.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SignatureCheckRefusesDrift(int* OutNativeWidth)
    {
        void* Agreeing = NativeBindings.Resolve(Host.NativeLibrary, "LuminaSharp_PropertyOffset", 12);
        void* Drifted = NativeBindings.Resolve(Host.NativeLibrary, "LuminaSharp_PropertyOffset", 16);
        void* Unchecked = NativeBindings.Resolve(Host.NativeLibrary, "LuminaSharp_PropertyOffset");

        *OutNativeWidth = 12;
        return (Agreeing != null ? 1 : 0) | (Drifted == null ? 2 : 0) | (Unchecked != null ? 4 : 0);
    }
}
