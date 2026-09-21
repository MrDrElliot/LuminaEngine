using System;
using System.Collections.Generic;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace LuminaSharp;

// A frame is a container like an object is, so a blittable slot is copied in place and the rest is marshalled.
internal static unsafe class FrameMarshal
{
    internal readonly struct FSlot
    {
        internal FSlot(EElementKind Access, int Offset, IntPtr Property, Type Declared, FValueCopier? Copier,
            FViewFactory? View = null)
        {
            this.Access = Access;
            this.Offset = Offset;
            this.Property = Property;
            this.Declared = Declared;
            this.Copier = Copier;
            this.View = View;
        }

        internal readonly EElementKind  Access;
        internal readonly int           Offset;
        internal readonly IntPtr        Property;
        internal readonly Type          Declared;
        internal readonly FValueCopier? Copier;
        internal readonly FViewFactory? View;
    }

    // A container view is two words, the instance and its ops table, so one factory serves every closed form.
    internal sealed class FViewFactory
    {
        internal FViewFactory(Func<nint, nint, object> Make, IntPtr Ops,
            Action<nint, nint, object?>? Fill = null)
        {
            this.Make = Make;
            this.Ops = Ops;
            this.Fill = Fill;
        }

        private readonly Func<nint, nint, object>    Make;
        private readonly IntPtr                      Ops;
        private readonly Action<nint, nint, object?>? Fill;

        internal object Create(nint Address)
        {
            return Make(Address, (nint)Ops);
        }

        internal void Write(nint Address, object? Value)
        {
            Fill!(Address, (nint)Ops, Value);
        }
    }

    // The generic read and write for one value type, closed over that type once and reused per call.
    internal sealed class FValueCopier
    {
        internal FValueCopier(Func<nint, object> Read, Action<nint, object?> Write, int Size)
        {
            this.Read = Read;
            this.Write = Write;
            this.Size = Size;
        }

        internal readonly Func<nint, object>    Read;
        internal readonly Action<nint, object?> Write;
        internal readonly int                   Size;
    }

    private static readonly Dictionary<Type, FValueCopier?> CopierByType = new();

    private static readonly Dictionary<Type, Func<nint, nint, object>?> ViewFactoryByType = new();

    private static readonly MethodInfo ReadValueMethod =
        typeof(FrameMarshal).GetMethod(nameof(ReadValue), BindingFlags.NonPublic | BindingFlags.Static)!;

    private static readonly MethodInfo WriteValueMethod =
        typeof(FrameMarshal).GetMethod(nameof(WriteValue), BindingFlags.NonPublic | BindingFlags.Static)!;

    private static readonly MethodInfo SizeOfMethod =
        typeof(Unsafe).GetMethod(nameof(Unsafe.SizeOf), BindingFlags.Public | BindingFlags.Static)!;

    private static readonly MethodInfo HasReferencesMethod =
        typeof(RuntimeHelpers).GetMethod(nameof(RuntimeHelpers.IsReferenceOrContainsReferences),
            BindingFlags.Public | BindingFlags.Static)!;

    // Every one of these is keyed by a user type, so a Reset that misses one pins the unloading generation.
    internal static int CachedTypeCount => CopierByType.Count + ViewFactoryByType.Count;

    // C# in is by-ref but read-only, so only out and ref are slots the caller reads back.
    internal static EScriptParamFlags DirectionOf(ParameterInfo Parameter)
    {
        if (!Parameter.ParameterType.IsByRef || (Parameter.IsIn && !Parameter.IsOut))
        {
            return EScriptParamFlags.None;
        }

        return Parameter.IsOut
            ? EScriptParamFlags.OutParam
            : EScriptParamFlags.OutParam | EScriptParamFlags.RefParam;
    }

    // Resolved once per signature, so a mismatch drops the call instead of reading whatever is at the offset.
    internal static bool TryBind(IntPtr Property, Type Declared, string Where, out FSlot Slot)
    {
        Slot = default;

        // A frame slot holds the value, so out, ref and in all bind as the type behind the reference.
        if (Declared.IsByRef)
        {
            Declared = Declared.GetElementType()!;
        }

        if (Property == IntPtr.Zero)
        {
            Debug.LogError($"{Where}. The call frame has no slot for a {Declared.Name}, so the call is dropped.");
            return false;
        }

        int Offset = Native.PropertyOffset(Property);
        int Width = Native.PropertySize(Property);

        if (Offset < 0)
        {
            Debug.LogError($"{Where}. The {Declared.Name} slot has no offset, so the call is dropped.");
            return false;
        }

        EElementKind Kind = ElementKinds.Of(Declared);
        switch (Kind)
        {
            case EElementKind.Bool:
            case EElementKind.ManagedString:
                Slot = new FSlot(Kind, Offset, Property, Declared, null);
                return true;

            case EElementKind.ObjectWrapper:
                Slot = new FSlot(Kind, Offset, Property, Declared, null);
                return true;

            case EElementKind.Optional:
                return TryBindOptional(Property, Declared, Nullable.GetUnderlyingType(Declared)!, Offset, Where,
                    out Slot);

            case EElementKind.StructView:
            case EElementKind.SlotView:
            {
                Func<nint, nint, object>? MakeStruct = ResolveViewFactory(Declared);
                if (MakeStruct == null)
                {
                    Debug.LogError($"{Where}. {Declared.Name} has no view constructor to bind, so the call is "
                        + "dropped.");
                    return false;
                }

                Slot = new FSlot(Kind, Offset, Property, Declared, null, new FViewFactory(MakeStruct, IntPtr.Zero));
                return true;
            }

            case EElementKind.Vector:
            case EElementKind.Map:
                return TryBindView(Property, Declared, Kind, Offset, Where, out Slot);

            case EElementKind.VectorCopy:
                return TryBindCopy(Property, Declared, Offset, Where, out Slot);

            // The second word is the property, since the optional ops hang off it rather than an ops table.
            case EElementKind.OptionalView:
            {
                Func<nint, nint, object>? MakeOptional = ResolveViewFactory(Declared);
                if (MakeOptional == null || Native.PropOptionalInner(Property) == IntPtr.Zero)
                {
                    Debug.LogError($"{Where}. {Declared.Name} needs an optional frame slot with a view "
                        + "constructor, so the call is dropped.");
                    return false;
                }

                Slot = new FSlot(Kind, Offset, Property, Declared, null,
                    new FViewFactory(MakeOptional, Property));
                return true;
            }

            // Kinds the element marshaller carries for a container slot that a frame has never bound.
            case EElementKind.String:
            case EElementKind.ObjectRef:
            case EElementKind.SoftRef:
                Debug.LogError($"{Where}. {Declared.Name} is carried as a container element but not yet across "
                    + "a call frame, so the call is dropped.");
                return false;
        }

        FValueCopier? Copier = ResolveCopier(Declared);
        if (Copier == null)
        {
            Debug.LogError($"{Where}. {Declared.Name} cannot be moved through a call frame, so the call is "
                + "dropped. Supported are numbers, bool, enums, string, engine struct mirrors such as "
                + "FVector3 and FName, entity handles, object references and component views.");
            return false;
        }

        if (Copier.Size != Width)
        {
            Debug.LogError($"{Where}. {Declared.Name} is {Copier.Size} bytes in C# but its frame slot is "
                + $"{Width}, so the call is dropped rather than writing past it.");
            return false;
        }

        Slot = new FSlot(EElementKind.Blittable, Offset, Property, Declared, Copier);
        return true;
    }

    internal static object? Read(IntPtr Frame, in FSlot Slot)
    {
        nint Address = (nint)Frame + Slot.Offset;

        switch (Slot.Access)
        {
            case EElementKind.Blittable:
                return Slot.Copier!.Read(Address);

            case EElementKind.Bool:
                return Unsafe.ReadUnaligned<byte>((void*)Address) != 0;

            // An FString is read in place, exactly as a blittable one is; there is no crossing for it.
            case EElementKind.ManagedString:
                return NativeMarshal.ReadString(Address);

            case EElementKind.ObjectWrapper:
            {
                IntPtr Object = Native.PropGetObject(Frame, Slot.Property);
                return Object == IntPtr.Zero ? null : NativeObjectMarshal.FromHandleOfType(Object, Slot.Declared);
            }

            // The frame outlives the call, which is the whole window this view over its slot is valid for.
            case EElementKind.StructView:
            case EElementKind.SlotView:
            case EElementKind.VectorCopy:
            case EElementKind.OptionalView:
                return Slot.View!.Create(Address);

            // Native hands back a null payload for an unset optional, which is the null a nullable wants.
            case EElementKind.Optional:
            {
                IntPtr Payload = Native.PropOptionalGetValue(Frame, Slot.Property);
                return Payload == IntPtr.Zero ? null : Slot.Copier!.Read(Payload);
            }

            case EElementKind.Vector:
            case EElementKind.Map:
                return Slot.View!.Create(Address);
        }

        return null;
    }

    internal static void Write(IntPtr Frame, in FSlot Slot, object? Value)
    {
        nint Address = (nint)Frame + Slot.Offset;

        switch (Slot.Access)
        {
            case EElementKind.Blittable:
                Slot.Copier!.Write(Address, Value);
                return;

            case EElementKind.Bool:
                Unsafe.WriteUnaligned((void*)Address, (byte)(Value is true ? 1 : 0));
                return;

            // Assigned rather than written in place, since the frame's string owns its memory.
            case EElementKind.ManagedString:
                Native.PropSetString(Frame, Slot.Property, Value as string ?? string.Empty);
                return;

            case EElementKind.ObjectWrapper:
                Native.PropSetObject(Frame, Slot.Property, NativeObjectMarshal.ToHandle(Value as NativeObject));
                return;

            // The property owns the copy, since only it knows whether the struct's members own memory.
            case EElementKind.StructView:
                Native.PropCopyStruct(Frame, Slot.Property, NativeObjectMarshal.ToHandle(Value as NativeStruct));
                return;

            case EElementKind.Optional:
            {
                if (Value == null)
                {
                    Native.PropOptionalReset(Frame, Slot.Property);
                    return;
                }

                // Long-aligned, since the setter reads the payload back through its own type.
                long* Scratch = stackalloc long[(Slot.Copier!.Size + 7) / 8];
                Slot.Copier.Write((nint)Scratch, Value);
                Native.PropOptionalSetValue(Frame, Slot.Property, (IntPtr)Scratch);
                return;
            }

            // The frame owns its container, so a copy is emptied and refilled rather than aliased.
            case EElementKind.VectorCopy:
                Slot.View!.Write(Address, Value);
                return;

            // IsViewOnly keeps a view out of the write-back set, so reaching this means that gate was bypassed.
            case EElementKind.Vector:
            case EElementKind.Map:
                Debug.LogError($"A {Slot.Declared.Name} view aliases its frame slot and has no value to write "
                    + "back, so the write is dropped.");
                return;
        }
    }

    // A container view aliases the slot rather than copying it, so the callee's edits are already in the frame.
    internal static bool IsViewOnly(in FSlot Slot)
    {
        return Slot.Access is EElementKind.Vector or EElementKind.Map or EElementKind.SlotView
            or EElementKind.OptionalView;
    }

    // A copier closes over a user type, so leaving one cached would pin the generation being unloaded.
    internal static void Reset()
    {
        lock (CopierByType)
        {
            CopierByType.Clear();
        }

        lock (ViewFactoryByType)
        {
            ViewFactoryByType.Clear();
        }
    }

    // A list or array over a vector slot, bound to the copy helpers rather than to a view over the storage.
    private static bool TryBindCopy(IntPtr Property, Type Declared, int Offset, string Where, out FSlot Slot)
    {
        Slot = default;

        IntPtr Ops = Native.PropVectorOps(Property);
        if (Ops == IntPtr.Zero)
        {
            Debug.LogError($"{Where}. {Declared.Name} needs a vector frame slot and this one is not, so the "
                + "call is dropped.");
            return false;
        }

        Type Element = Declared.IsArray ? Declared.GetElementType()! : Declared.GetGenericArguments()[0];
        string Reader = Declared.IsArray ? nameof(VectorCopy.ToArray) : nameof(VectorCopy.ToList);

        var Make = (Func<nint, nint, object>)typeof(VectorCopy).GetMethod(Reader)!
            .MakeGenericMethod(Element).CreateDelegate(typeof(Func<nint, nint, object>));
        var Fill = (Action<nint, nint, object?>)typeof(VectorCopy).GetMethod(nameof(VectorCopy.Fill))!
            .MakeGenericMethod(Element).CreateDelegate(typeof(Action<nint, nint, object?>));

        Slot = new FSlot(EElementKind.VectorCopy, Offset, Property, Declared, null,
            new FViewFactory(Make, Ops, Fill));
        return true;
    }

    private static bool TryBindView(IntPtr Property, Type Declared, EElementKind Kind, int Offset, string Where,
        out FSlot Slot)
    {
        Slot = default;

        bool bMap = Kind == EElementKind.Map;
        IntPtr Ops = bMap ? Native.PropMapOps(Property) : Native.PropVectorOps(Property);

        if (Ops == IntPtr.Zero)
        {
            Debug.LogError($"{Where}. {Declared.Name} needs a {(bMap ? "map" : "vector")} frame slot and this "
                + "one is not, so the call is dropped.");
            return false;
        }

        Func<nint, nint, object>? Make = ResolveViewFactory(Declared);
        if (Make == null)
        {
            Debug.LogError($"{Where}. {Declared.Name} has no view constructor to bind, so the call is dropped.");
            return false;
        }

        Slot = new FSlot(Kind, Offset, Property, Declared, null, new FViewFactory(Make, Ops));
        return true;
    }

    private static Func<nint, nint, object>? ResolveViewFactory(Type Declared)
    {
        lock (ViewFactoryByType)
        {
            if (ViewFactoryByType.TryGetValue(Declared, out Func<nint, nint, object>? Cached))
            {
                return Cached;
            }

            Func<nint, nint, object>? Make = BuildViewFactory(Declared);
            ViewFactoryByType[Declared] = Make;
            return Make;
        }
    }

    // Compiled rather than invoked reflectively, which cost an object[] and a box per argument per call.
    private static Func<nint, nint, object>? BuildViewFactory(Type Declared)
    {
        // A container view takes its ops table as the second word; a struct view takes the address alone.
        ConstructorInfo? Pair = Declared.GetConstructor(new[] { typeof(nint), typeof(nint) });
        ConstructorInfo? Single = Pair != null
            ? null
            : Declared.GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                null, new[] { typeof(IntPtr) }, null);

        if (Pair == null && Single == null)
        {
            return null;
        }

        ParameterExpression Instance = Expression.Parameter(typeof(nint), "Instance");
        ParameterExpression Ops = Expression.Parameter(typeof(nint), "Ops");
        NewExpression Made = Pair != null
            ? Expression.New(Pair, Instance, Ops)
            : Expression.New(Single!, Instance);

        return Expression.Lambda<Func<nint, nint, object>>(
            Expression.Convert(Made, typeof(object)), Instance, Ops).Compile();
    }

    // A nullable crosses as its payload, matching how a TOptional property already binds.
    private static bool TryBindOptional(IntPtr Property, Type Declared, Type Payload, int Offset, string Where,
        out FSlot Slot)
    {
        Slot = default;

        IntPtr Inner = Native.PropOptionalInner(Property);
        if (Inner == IntPtr.Zero)
        {
            Debug.LogError($"{Where}. {Declared.Name} needs an optional frame slot and this one is not, so the "
                + "call is dropped.");
            return false;
        }

        FValueCopier? Copier = ResolveCopier(Payload);
        if (Copier == null)
        {
            Debug.LogError($"{Where}. {Payload.Name} cannot be the payload of an optional crossing a call "
                + "frame, so the call is dropped.");
            return false;
        }

        int Width = Native.PropertySize(Inner);
        if (Copier.Size != Width)
        {
            Debug.LogError($"{Where}. {Payload.Name} is {Copier.Size} bytes in C# but the optional's payload "
                + $"is {Width}, so the call is dropped rather than writing past it.");
            return false;
        }

        Slot = new FSlot(EElementKind.Optional, Offset, Property, Declared, Copier);
        return true;
    }

    private static object ReadValue<T>(nint Address) where T : unmanaged
    {
        return Unsafe.ReadUnaligned<T>((void*)Address);
    }

    private static void WriteValue<T>(nint Address, object? Value) where T : unmanaged
    {
        Unsafe.WriteUnaligned((void*)Address, Value is T Typed ? Typed : default);
    }

    private static FValueCopier? ResolveCopier(Type Declared)
    {
        lock (CopierByType)
        {
            if (CopierByType.TryGetValue(Declared, out FValueCopier? Cached))
            {
                return Cached;
            }

            FValueCopier? Copier = BuildCopier(Declared);
            CopierByType[Declared] = Copier;
            return Copier;
        }
    }

    // Null for a type that has to be marshalled rather than copied.
    private static FValueCopier? BuildCopier(Type Declared)
    {
        if (!Declared.IsValueType || Declared == typeof(void) || Declared.ContainsGenericParameters
            || Declared.IsByRefLike || Declared.IsPointer)
        {
            return null;
        }

        try
        {
            // Reflection does not enforce ReadValue's unmanaged constraint, so this is the real gate.
            if ((bool)HasReferencesMethod.MakeGenericMethod(Declared).Invoke(null, null)!)
            {
                return null;
            }

            var Read = (Func<nint, object>)ReadValueMethod.MakeGenericMethod(Declared)
                .CreateDelegate(typeof(Func<nint, object>));

            var Write = (Action<nint, object?>)WriteValueMethod.MakeGenericMethod(Declared)
                .CreateDelegate(typeof(Action<nint, object?>));

            int Size = (int)SizeOfMethod.MakeGenericMethod(Declared).Invoke(null, null)!;

            return new FValueCopier(Read, Write, Size);
        }
        catch (Exception Exception)
        {
            Debug.LogError($"{Declared.Name} could not be bound as a blittable call-frame value. {Exception.Message}");
            return null;
        }
    }
}
