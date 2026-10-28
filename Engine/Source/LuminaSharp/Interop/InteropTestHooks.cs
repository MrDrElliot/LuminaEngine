using System;
using System.Linq;
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
internal sealed class FrameMarshalTarget : NativeObject
{
    [Function]
    public void MarshalOut(int In, out int Out)
    {
        Out = In * 2;
    }

    [Function]
    public void MarshalRef(ref int Value)
    {
        Value += 5;
    }

    // A string parameter, which used to have no generated invoker and fell to the boxing reflection path.
    [Function]
    public void MarshalText(string In, out int Length)
    {
        Length = In == null ? -1 : In.Length;
    }

    // An opaque struct wrapper, which stays on the reflection path and is read through a view over the slot.
    [Function]
    public void MarshalStructView(Lumina.SAnimGraphBoneMaskBone Bone, out float Weight)
    {
        Weight = Bone.Ref.IsNull ? -1.0f : Bone.Weight;
    }

    // An async body that yields once, so the token is observably running before the tick completes it.
    [Function]
    public async System.Threading.Tasks.Task MarshalAsync(int Value)
    {
        AsyncGate = new System.Threading.Tasks.TaskCompletionSource();
        await AsyncGate.Task;
        AsyncSeen = Value;
    }

    internal static System.Threading.Tasks.TaskCompletionSource? AsyncGate;
    internal static int AsyncSeen;

    // A delegate and an instanced struct name the slot they view, which the frame binder used to refuse.
    [Function]
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
    [Function]
    public void Ambiguous(int Value)
    {
    }

    [Function]
    public void Ambiguous(float Value)
    {
    }

    [Function]
    public void Unambiguous()
    {
    }
}

// A minted parent whose stored property a subclass overrides, so the override has to reuse the parent's storage.
internal class OverrideSchemaParent : NativeObject
{
    [Property]
    public virtual string Label { get; set; } = "";

    [Property]
    public int Untouched { get; set; }
}

internal sealed class OverrideSchemaChild : OverrideSchemaParent
{
    [Property]
    public override string Label { get; set; } = "Child";

    [Property]
    public int OwnField { get; set; }
}

// An abstract property has no storage, so the override that implements it is the one that stores it.
internal abstract class AbstractSchemaParent : NativeObject
{
    public abstract int Count { get; set; }
}

internal sealed class AbstractSchemaChild : AbstractSchemaParent
{
    [Property]
    public override int Count { get; set; }
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
        if (!FrameMarshal.TryBind(Property, typeof(FEntity), "Test_FrameReadEntity", out FrameMarshal.FSlot Slot))
        {
            return 0;
        }

        *OutId = ((FEntity)FrameMarshal.Read(Frame, Slot)!).Id;
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
        ScriptType Resolved = Library.ResolveType(typeof(float?));

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
        var Values = CInteropTestLibrary.MakeEntityRange(Count);
        *OutLast = Values.Length > 0 ? Values[Values.Length - 1].Id : 0;
        return Values.Length;
    }

    // An out container of objects is rebuilt through the wrapper cache, so each element comes back as the same instance.
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
        Lumina.SUIElement Handle = CInteropTestLibrary.MakeHandle(Value);
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
        return ScriptCallback.Of((FEntity Spawned) =>
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
        return ScriptCallback.OfFloat((float Value) =>
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

    // How many functions survive describing a type whose [Function] names are not all distinct.
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

    // How many times Name lands in Child's schema below MintedParent, which is what its minted class adds to the parent's.
    private static int SchemaCount(Type Child, Type MintedParent, string Name)
    {
        var Library = new TypeLibrary(new[] { MintedParent, Child });
        TypeDescription Description = new(Child);
        Description.Build(Library);
        return Serializer.PropertiesBelow(Description, MintedParent).Count(Property => Property.Name == Name);
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_OverriddenPropertySchemaCounts(int* OutOverride, int* OutOwn, int* OutInherited, int* OutAbstractOverride)
    {
        *OutOverride = SchemaCount(typeof(OverrideSchemaChild), typeof(OverrideSchemaParent), nameof(OverrideSchemaChild.Label));
        *OutOwn = SchemaCount(typeof(OverrideSchemaChild), typeof(OverrideSchemaParent), nameof(OverrideSchemaChild.OwnField));
        *OutInherited = SchemaCount(typeof(OverrideSchemaChild), typeof(OverrideSchemaParent), nameof(OverrideSchemaParent.Untouched));
        *OutAbstractOverride = SchemaCount(typeof(AbstractSchemaChild), typeof(AbstractSchemaParent), nameof(AbstractSchemaChild.Count));
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

        FrameMarshal.TryBind(StructProperty, typeof(Lumina.SAnimGraphBoneMaskBone), "CacheProbe", out _);
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
            (typeof(Lumina.SAnimGraphBoneMaskBone), EElementKind.StructView),
            (typeof(Lumina.CWorld), EElementKind.ObjectWrapper),
            (typeof(Lumina.TStrongObjectPtr<Lumina.CWorld>), EElementKind.ObjectRef),

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
        var Opaque = new Lumina.SInteropOpaqueStruct(Storage);

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
        var Opaque = new Lumina.SInteropOpaqueStruct(Storage);

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

        public ulong World => 0;
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
        Lumina.SInteropOpaqueStruct Opaque = Storage == IntPtr.Zero
            ? default
            : new Lumina.SInteropOpaqueStruct(Storage);

        return CInteropTestLibrary.ReadOpaqueValue(Opaque);
    }

    private static IntPtr DelegateProbeHost;
    private static int DelegateProbeFlags;

    private static void OnProbeOne(string Text)
    {
        DelegateProbeFlags |= 1;
        if (Text == "solo") DelegateProbeFlags |= 2;
    }

    private static void OnProbeTwo(Lumina.SDelegateTestPayload Payload, float Scalar)
    {
        DelegateProbeFlags |= 4;
        if (Payload.X == 2.0f && Payload.Count == 5 && Scalar == 0.25f) DelegateProbeFlags |= 8;
    }

    private static void OnProbeThree(Lumina.SDelegateTestPayload Payload, float Scalar, string Text)
    {
        DelegateProbeFlags |= 16;
        if (Payload.X == 7.0f && Payload.Count == 3 && Scalar == 1.5f && Text == "hello") DelegateProbeFlags |= 32;
    }

    // Subscribes to every arity the generated events expose, over a native SDelegateTestHost the caller owns.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_DelegateArgsBind(IntPtr Host)
    {
        DelegateProbeFlags = 0;
        DelegateProbeHost = Host;

        Lumina.SDelegateTestHost View = new Lumina.SDelegateTestHost(Host);
        View.OnOneString += OnProbeOne;
        View.OnTwo += OnProbeTwo;
        View.OnThree += OnProbeThree;
        return 1;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_DelegateArgsFlags() => DelegateProbeFlags;

    // Must run while the native delegates are still alive, since -= reads their listeners.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_DelegateArgsRelease()
    {
        Lumina.SDelegateTestHost View = new Lumina.SDelegateTestHost(DelegateProbeHost);
        View.OnOneString -= OnProbeOne;
        View.OnTwo -= OnProbeTwo;
        View.OnThree -= OnProbeThree;
        DelegateProbeHost = IntPtr.Zero;
    }

    // A handler added twice and removed once stays bound once, and a handler never added removes nothing.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_DelegateArgsAddTwiceRemoveOnce(IntPtr Host)
    {
        Lumina.SDelegateTestHost View = new Lumina.SDelegateTestHost(Host);
        View.OnTwo += OnProbeTwo;
        View.OnTwo += OnProbeTwo;
        View.OnTwo -= OnProbeTwo;
        View.OnOneString -= OnProbeOne;
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

    // Writes two render-facing fields through the generated setters, which must tell the component it changed.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_WriteStaticMeshComponent(IntPtr Component)
    {
        ref SStaticMeshComponent View = ref Unsafe.AsRef<SStaticMeshComponent>((void*)Component);
        View.StaticMesh = null!;
        View.bCastShadow = false;
    }

    // Failure bits, four per form, for a root, an extension and a component compiled together the way the editor compiles them.
    // Script structs picked by the interface they implement, the strategy pattern C# structs reach without inheritance.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ScriptStructStrategies()
    {
        const string Source = """
            using Lumina;
            using LuminaSharp;
            public interface IProbeStrategy
            {
                int Compute(int X);
            }
            [Reflect]
            public partial struct DoubleStrategy : INativeStruct<DoubleStrategy>, IProbeStrategy
            {
                [Property] public int Unused;
                public int Compute(int X) => X * 2;
            }
            [Reflect]
            public partial struct AddStrategy : INativeStruct<AddStrategy>, IProbeStrategy
            {
                [Property] public int Unused;
                public int Compute(int X) => X + 3;
            }
            [Reflect]
            public partial struct PlainProbeData : INativeStruct<PlainProbeData>
            {
                [Property] public int Value;
            }
            """;
        FScriptImage? Image = ScriptCompiler.Compile("StrategyProbe", new[] { ("StrategyProbe.cs", Source) });
        if (Image is not { } Compiled)
        {
            return -1;
        }

        var Context = new ScriptLoadContext("StrategyProbe");
        void* Memory = System.Runtime.InteropServices.NativeMemory.AllocZeroed(64);
        try
        {
            Type[] Types = Context.LoadScriptAssembly("StrategyProbe", Compiled).GetTypes();
            var Library = new TypeLibrary(Types);
            int Failures = 0;

            Type Interface = Types.First(Candidate => Candidate.Name == "IProbeStrategy");
            System.Reflection.MethodInfo Compute = Interface.GetMethod("Compute")!;
            int Run(string Name) => Library.ViewScriptStruct(Name, (nint)Memory) is { } View && Interface.IsInstanceOfType(View)
                ? (int)Compute.Invoke(View, new object[] { 5 })!
                : int.MinValue;

            Failures |= Run("DoubleStrategy") == 10 ? 0 : 1;
            Failures |= Run("AddStrategy") == 8 ? 0 : 2;
            Failures |= Run("PlainProbeData") == int.MinValue ? 0 : 4;
            Failures |= Library.ViewScriptStruct("NoSuchStruct", (nint)Memory) == null ? 0 : 8;
            Failures |= Library.ViewScriptStruct("DoubleStrategy", 0) == null ? 0 : 16;

            Type Double = Types.First(Candidate => Candidate.Name == "DoubleStrategy");
            Type Plain = Types.First(Candidate => Candidate.Name == "PlainProbeData");
            Failures |= TypeLibrary.ImplementedInterfaces(Double) == "IProbeStrategy" ? 0 : 32;
            Failures |= TypeLibrary.ImplementedInterfaces(Plain) == "" ? 0 : 64;
            return Failures;
        }
        catch (Exception Error)
        {
            Native.Log(ELogLevel.Error, "StrategyProbe threw " + Error);
            return -2;
        }
        finally
        {
            System.Runtime.InteropServices.NativeMemory.Free(Memory);
            Context.Unload();
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_CompileScriptStructForms()
    {
        const string Source = """
            using Lumina;
            using LuminaSharp;
            [Reflect]
            public partial struct RootProbeData : INativeStruct<RootProbeData>
            {
                [Property] public float MoveSpeed = 2f;
                public RootProbeData() { }
            }
            [Reflect]
            public partial struct RowProbeData : IExtends<SDataTableRowBase>
            {
                [Property] public int Count;
            }
            [Reflect]
            public partial struct ComponentProbeData : IComponent<ComponentProbeData>
            {
                [Property] public float Health;
            }
            """;
        FScriptImage? Image = ScriptCompiler.Compile("StructFormProbe", new[] { ("StructFormProbe.cs", Source) });
        if (Image is not { } Compiled)
        {
            return -1;
        }

        var Context = new ScriptLoadContext("StructFormProbe");
        try
        {
            Type[] Types = Context.LoadScriptAssembly("StructFormProbe", Compiled).GetTypes();
            var Registered = new TypeLibrary(Types).ScriptStructTypes.ToList();
            int Failures = Registered.Count == 3 ? 0 : 1 << 12;
            (string Name, string Base, bool bHandle)[] Forms =
            {
                ("RootProbeData", "", true),
                ("RowProbeData", "SDataTableRowBase", true),
                ("ComponentProbeData", "", false),
            };
            for (int i = 0; i < Forms.Length; ++i)
            {
                Type? Form = Types.FirstOrDefault(Candidate => Candidate.Name == Forms[i].Name);
                if (Form == null)
                {
                    Failures |= 0xF << (i * 4);
                    continue;
                }
                if (!TypeLibrary.IsNativeBackedStruct(Form))
                {
                    Failures |= 1 << (i * 4);
                }
                if (Registered.Count(Pair => Pair.Key == Forms[i].Name && Pair.Value.NativeBase == Forms[i].Base) != 1)
                {
                    Failures |= 2 << (i * 4);
                }
                bool bOneHandle = Form.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic) is [{ FieldType: var Only }] && Only == typeof(NativeRef);
                int SelfHandles = Form.GetInterfaces().Count(Interface => Interface.IsGenericType && Interface.GetGenericTypeDefinition() == typeof(INativeStruct<>) && Interface.GetGenericArguments()[0] == Form);
                if (bOneHandle != Forms[i].bHandle || SelfHandles != (Forms[i].bHandle ? 1 : 0))
                {
                    Failures |= 4 << (i * 4);
                }
                if (Form.GetProperty("NativeName", BindingFlags.Public | BindingFlags.Static)?.GetValue(null) as string != Forms[i].Name)
                {
                    Failures |= 8 << (i * 4);
                }
            }
            return Failures;
        }
        catch (Exception Error)
        {
            Native.Log(ELogLevel.Error, "StructFormProbe threw " + Error);
            return -2;
        }
        finally
        {
            Context.Unload();
        }
    }

    // Failure bits for Get and TryGet on an instanced struct holding an SInteropOpaqueStruct, and on an unbound one.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_InstancedStructGetTryGet(IntPtr Address, float NewValue)
    {
        var Instanced = new Lumina.FInstancedStruct(Address);
        int Failures = 0;
        if (Instanced.TryGet<Lumina.SInteropOpaqueStruct>() is not { IsValid: true })
        {
            Failures |= 1;
        }
        Instanced.Get<Lumina.SInteropOpaqueStruct>().Value = NewValue / 2;
        Instanced.TryGet<Lumina.SInteropOpaqueStruct>()?.Value = NewValue;
        if (Instanced.TryGet<Lumina.SDataTableRowBase>() != null)
        {
            Failures |= 2;
        }
        if (!Throws(() => Instanced.Get<Lumina.SDataTableRowBase>()))
        {
            Failures |= 4;
        }
        var Unbound = new Lumina.FInstancedStruct(0);
        if (Unbound.TryGet<Lumina.SInteropOpaqueStruct>() != null || !Throws(() => Unbound.Get<Lumina.SInteropOpaqueStruct>()))
        {
            Failures |= 8;
        }
        return Failures;

        static bool Throws(Action Call)
        {
            try
            {
                Call();
                return false;
            }
            catch (InvalidOperationException)
            {
                return true;
            }
        }
    }

    // On its first row the walk removes the doomed rows' second component and grows that pool, then sums what every row reads.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ViewMutatesMidLoop(IntPtr World, uint* Doomed, int DoomedCount, uint* Joiners, int JoinerCount, uint* OutFirst, double* OutSum)
    {
        var Registry = new Lumina.CWorld(World);
        int Visited = 0;
        foreach (var Row in Registry.View<Lumina.STransformComponent, Lumina.SBuoyancyComponent>().Each())
        {
            // Read first, since a structural change leaves even the current row's refs dangling.
            *OutSum += Row.Second.Buoyancy;
            if (Visited == 0)
            {
                *OutFirst = Row.Entity.Id;
                for (int i = 0; i < DoomedCount; ++i)
                {
                    if (Doomed[i] != Row.Entity.Id)
                    {
                        Registry.RemoveComponent<Lumina.SBuoyancyComponent>(new FEntity(Doomed[i]));
                    }
                }
                for (int i = 0; i < JoinerCount; ++i)
                {
                    Registry.GetOrEmplaceComponent<Lumina.SBuoyancyComponent>(new FEntity(Joiners[i])).Buoyancy = -1000.0f;
                }
            }
            ++Visited;
        }
        return Visited;
    }

    // Finds a C++ subsystem through the generic lookup, which must hand it back as its own type.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_GetSubsystemTyped(IntPtr World)
    {
        return new Lumina.CWorld(World).GetSubsystem<Lumina.CSpawnSubsystem>() != null ? 1 : 0;
    }

    // Spawns a loaded prefab through the World overload, placed under a parent.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static uint Test_SpawnLoadedPrefab(IntPtr World, IntPtr Prefab, uint Parent)
    {
        var Target = new Lumina.CWorld(World);
        var Loaded = Wrapper<Lumina.CPrefab>.ForObject(Prefab)!;
        return Target.Spawner.SpawnPrefab(Loaded, new FVector3(3.0f, 4.0f, 5.0f), null, new FEntity(Parent)).Id;
    }

    // Reads the row named Row through SInteropTestRow, by name and through a handle, and -1 when either refuses.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ReadRowAsBase(IntPtr Table, IntPtr Handle)
    {
        var Loaded = Wrapper<Lumina.CDataTable>.ForObject(Table)!;
        Lumina.SInteropTestRow? ByName = Loaded.TryGetRow<Lumina.SInteropTestRow>("Row");
        Lumina.SInteropTestRow? ByHandle = NativeStructs.FromAddress<Lumina.SDataTableRowHandle>(Handle).TryGet<Lumina.SInteropTestRow>();
        if (ByName is not { } Row || ByHandle is not { } Same)
        {
            return -1;
        }
        return Row.BaseValue == Same.BaseValue ? Row.BaseValue : -1;
    }

    // True when the table refuses to be read as the derived row, both by name and through a handle.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_RefusesRowAsDerived(IntPtr Table, IntPtr Handle)
    {
        var Loaded = Wrapper<Lumina.CDataTable>.ForObject(Table)!;
        return Loaded.TryGetRow<Lumina.SInteropTestDerivedRow>("Row") is null
            && NativeStructs.FromAddress<Lumina.SDataTableRowHandle>(Handle).TryGet<Lumina.SInteropTestDerivedRow>() is null ? 1 : 0;
    }

    private static FVector3 TweenValue;
    private static int TweenFinished;
    private static int TweenLoops;

    // Builds one tween through the fluent C# API, for native to tick and Test_TweenRead to inspect.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_TweenBuild(IntPtr World, uint Entity, float* OutDuration)
    {
        var Target = new Lumina.CWorld(World);
        FEntity E = new FEntity(Entity);
        TweenValue = default;
        TweenFinished = 0;
        TweenLoops = 0;

        Tween T = Target.Tweens.Create(E)
            .MoveTo(E, new FVector3(3.0f, 0.0f, 0.0f), 0.2f).Trans(EEaseTransition.Linear)
            .Parallel()
            .Value(new FVector3(0.0f), new FVector3(1.0f, 2.0f, 3.0f), 0.2f, V => TweenValue = V).Trans(EEaseTransition.Linear)
            .Property<Lumina.SBuoyancyComponent>(E, "Buoyancy", 5.0f, 0.2f).Trans(EEaseTransition.Linear)
            .Punch(E, ETweenTransformChannel.Scale, new FVector3(0.5f), 0.2f)
            .SetLoops(2, ETweenLoopMode.Yoyo)
            .OnLoop(Count => TweenLoops = Count)
            .OnFinished(() => ++TweenFinished);

        *OutDuration = T.Duration;
        return T.IsRunning ? 1 : 0;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_TweenRead(float* OutValue, int* OutFinished, int* OutLoops)
    {
        OutValue[0] = TweenValue.X;
        OutValue[1] = TweenValue.Y;
        OutValue[2] = TweenValue.Z;
        *OutFinished = TweenFinished;
        *OutLoops = TweenLoops;
    }

    // Reads what native queued through fresh readers, then sends three script pings for the next hook to read back.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_EventsReadEngineAndSend(IntPtr World, int* OutJumpSum, int* OutSlotLength, int* OutUnreadAfter)
    {
        WorldEvents Events = new Lumina.CWorld(World).Events;

        EventReader<SCharacterJumpedEvent> Jumps = default;
        int Count = 0;
        foreach (ref readonly SCharacterJumpedEvent Jump in Events.Read(ref Jumps))
        {
            *OutJumpSum += Jump.JumpCount;
            ++Count;
        }
        *OutUnreadAfter = Events.HasUnread(in Jumps) ? 1 : 0;

        EventReader<SSaveGameCompletedEvent> Completions = default;
        foreach (SSaveGameCompletedEvent Completed in Events.ReadViews(ref Completions))
        {
            *OutSlotLength += Completed.SlotName.Length;
        }

        ReadOnlySpan<TestScriptPing> Pings = stackalloc TestScriptPing[] { new() { Value = 1 }, new() { Value = 2 }, new() { Value = 3, Scale = 0.5 } };
        Events.Send(Pings);
        return Count;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_EventsReadScript(IntPtr World, double* OutScaleSum)
    {
        WorldEvents Events = new Lumina.CWorld(World).Events;
        EventReader<TestScriptPing> Reader = default;
        int Sum = 0;
        foreach (ref readonly TestScriptPing Ping in Events.Read(ref Reader))
        {
            Sum += Ping.Value;
            *OutScaleSum += Ping.Scale;
        }
        return Sum;
    }
}

// A script event with no native counterpart, laid out from C# alone and padded so its alignment is not four.
internal struct TestScriptPing : IEvent
{
    public int Value;
    public double Scale;
}
