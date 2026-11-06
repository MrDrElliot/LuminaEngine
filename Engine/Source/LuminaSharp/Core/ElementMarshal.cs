using System;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.CompilerServices;
using Lumina;

namespace LuminaSharp;

/// <summary>How a container element is carried between native storage and C#.</summary>
internal enum EElementKind
{
    /// <summary>The managed value IS the native bytes. Read and written in place; the fast path.</summary>
    Blittable,

    /// <summary>A native <c>FString</c>. Read by decoding in place, written through native assignment.</summary>
    String,

    // A reference-counted object slot, read as a raw pointer and written through the native assignment.
    ObjectRef,

    /// <summary>An opaque reflected struct. Read as a wrapper viewing the slot, written by native struct copy.</summary>
    StructView,

    /// <summary>An asset reference. Stored natively as one FSoftObjectPath, so the path is the whole value.</summary>
    SoftRef,

    // Native writes any nonzero byte, so the read normalizes rather than reinterpreting the slot.
    Bool,

    // A managed string over the same FString storage FString names, decoded rather than handled.
    ManagedString,

    // A CObject wrapper over a TStrongObjectPtr slot, resolved through the managed-instance cache so identity holds.
    ObjectWrapper,

    // The managed value is the slot address, so the slot is viewed in place and never copied over it.
    SlotView,

    // A managed list or array over a vector slot, copied rather than aliased, so a frame can return one.
    VectorCopy,

    // A view over an optional slot, for a payload a Nullable cannot spell.
    OptionalView,

    // The three below alias their slot and need the token their position resolves, so no address-only form works.
    Optional,

    Vector,

    Map,

    // A struct declared in script that holds a string, read and written field by field at its native offsets.
    ScriptStruct,
}

// The element kind of one T, resolved once, a JIT constant so TVector of a plain value reads memory directly.
internal static class ElementKind<T>
{
    public static readonly EElementKind Kind = ElementKinds.Of(typeof(T));

    // True for the kinds whose managed value is not the native bytes, so a ref into storage would be wrong.
    public static readonly bool IsMarshalled = Kind is not (EElementKind.Blittable or EElementKind.Bool);

    public static readonly Func<IntPtr, T>? MakeObject = BindObject();

    public static readonly Func<nint, T>? ReadScriptStruct = BindScriptStruct<Func<nint, T>>(nameof(ScriptStructMarshal<int>.Read));

    public static readonly Action<nint, T>? WriteScriptStruct = BindScriptStruct<Action<nint, T>>(nameof(ScriptStructMarshal<int>.Write));

    // ScriptStructMarshal carries the struct constraint, which T here does not.
    private static TDelegate? BindScriptStruct<TDelegate>(string Method) where TDelegate : Delegate
    {
        if (Kind != EElementKind.ScriptStruct)
        {
            return null;
        }
        return (TDelegate)typeof(ScriptStructMarshal<>).MakeGenericType(typeof(T)).GetMethod(Method)!.CreateDelegate(typeof(TDelegate));
    }

    public static readonly Func<nint, T>? MakeOptional = BindOptional<Func<nint, T>>(nameof(OptionalMarshal.Read));

    public static readonly Func<T, nint, bool>? StageOptional =
        BindOptional<Func<T, nint, bool>>(nameof(OptionalMarshal.Stage));

    public static readonly int OptionalPayloadSize = MeasureOptionalPayload();

    private static TDelegate? BindOptional<TDelegate>(string Method) where TDelegate : Delegate
    {
        if (Kind != EElementKind.Optional)
        {
            return null;
        }

        return (TDelegate)typeof(OptionalMarshal).GetMethod(Method)!
            .MakeGenericMethod(Nullable.GetUnderlyingType(typeof(T))!)
            .CreateDelegate(typeof(TDelegate));
    }

    private static int MeasureOptionalPayload()
    {
        if (Kind != EElementKind.Optional)
        {
            return 0;
        }

        return (int)typeof(OptionalMarshal).GetMethod(nameof(OptionalMarshal.SizeOf))!
            .MakeGenericMethod(Nullable.GetUnderlyingType(typeof(T))!)
            .Invoke(null, null)!;
    }

    public static readonly Func<nint, nint, T>? MakeContainerView = BuildContainerView();

    private static Func<IntPtr, T>? BindObject()
    {
        if (Kind != EElementKind.ObjectWrapper)
        {
            return null;
        }

        return (Func<IntPtr, T>)typeof(NativeObjectMarshal).GetMethod(nameof(NativeObjectMarshal.FromHandle))!
            .MakeGenericMethod(typeof(T))
            .CreateDelegate(typeof(Func<IntPtr, T>));
    }

    // Two words, the instance and its ops table, which is the shape every closed view constructor takes.
    private static Func<nint, nint, T>? BuildContainerView()
    {
        if (Kind is not (EElementKind.Vector or EElementKind.Map))
        {
            return null;
        }

        ConstructorInfo? Ctor = typeof(T).GetConstructor(new[] { typeof(nint), typeof(nint) });
        if (Ctor == null)
        {
            return null;
        }

        ParameterExpression Instance = Expression.Parameter(typeof(nint), "Instance");
        ParameterExpression Ops = Expression.Parameter(typeof(nint), "Ops");
        return Expression.Lambda<Func<nint, nint, T>>(Expression.New(Ctor, Instance, Ops), Instance, Ops).Compile();
    }

    /// <summary>Builds the slot view once per T, from the same (IntPtr) ctor Wrapper uses.</summary>
    public static readonly Func<nint, T>? MakeView = BuildView();

    /// <summary>The reflected struct name StructAssign resolves; empty for every other kind.</summary>
    public static readonly string NativeName = Kind == EElementKind.StructView ? NativeTypeName.Of<T>() : string.Empty;

    public static readonly Func<string, T>? MakeSoftReference = BindSoftReference<Func<string, T>>(nameof(SoftObjectReferenceMarshal.Read));

    public static readonly Func<T, string>? SoftReferencePath = BindSoftReference<Func<T, string>>(nameof(SoftObjectReferenceMarshal.Write));

    // Only SoftObjectReferenceMarshal carries the struct constraint that makes the interface calls constrained ones.
    private static TDelegate? BindSoftReference<TDelegate>(string Method) where TDelegate : Delegate
    {
        if (Kind != EElementKind.SoftRef || !typeof(T).IsValueType)
        {
            return null;
        }

        return (TDelegate)typeof(SoftObjectReferenceMarshal).GetMethod(Method)!
            .MakeGenericMethod(typeof(T))
            .CreateDelegate(typeof(TDelegate));
    }

    private static Func<nint, T>? BuildView()
    {
        // A struct handle is one NativeRef, so viewing an element is a reinterpretation rather than a constructor call.
        if (Kind == EElementKind.StructView)
        {
            return Address =>
            {
                NativeRef Ref = new(Address);
                return Unsafe.As<NativeRef, T>(ref Ref);
            };
        }
        if (Kind != EElementKind.SlotView)
        {
            return null;
        }

        ConstructorInfo? Ctor = typeof(T).GetConstructor(
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null, new[] { typeof(IntPtr) }, null);
        if (Ctor == null)
        {
            return null;
        }

        ParameterExpression Address = Expression.Parameter(typeof(nint), "Address");
        return Expression.Lambda<Func<nint, T>>(Expression.New(Ctor, Address), Address).Compile();
    }
}

/// <summary>Reads and writes one element at a native address, according to its <see cref="ElementKind{T}"/>.</summary>
public static unsafe class ElementMarshal
{
    /// <summary>The element at <paramref name="Address"/>.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static T Read<T>(nint Address)
    {
        switch (ElementKind<T>.Kind)
        {
            case EElementKind.String:
            {
                // Unsafe.As rather than a cast through object, since the kind guarantees T is FString and boxing would defeat the view.
                FString Value = new FString(Address);
                return Unsafe.As<FString, T>(ref Value);
            }
            case EElementKind.ObjectRef:
            {
                // Through the native Get, which answers null for an object the slot still names after it was destroyed.
                IntPtr Live = Native.GetObjectPtr(Address);
                return Unsafe.As<IntPtr, T>(ref Live);
            }

            case EElementKind.StructView:
            case EElementKind.SlotView:
                // The wrapper points at the slot, so a mutation through it edits the array in place.
                return ElementKind<T>.MakeView!(Address);

            case EElementKind.SoftRef:
                // Casting the local to ISoftObjectReference would mutate a box this then throws away, leaving the default.
                return ElementKind<T>.MakeSoftReference!(Native.SoftPathGet(Address));

            case EElementKind.ManagedString:
            {
                string Text = NativeMarshal.ReadString(Address);
                return Unsafe.As<string, T>(ref Text);
            }

            case EElementKind.Bool:
            {
                bool Value = Unsafe.ReadUnaligned<byte>((void*)Address) != 0;
                return Unsafe.As<bool, T>(ref Value);
            }

            // Through the native Get rather than the raw first word, or a slot that moved on reads as live.
            case EElementKind.ObjectWrapper:
                return ElementKind<T>.MakeObject!(Native.GetObjectPtr(Address));

            case EElementKind.ScriptStruct:
                return ElementKind<T>.ReadScriptStruct!(Address);

            case EElementKind.Optional:
            case EElementKind.Vector:
            case EElementKind.Map:
                Debug.LogError($"A {typeof(T).Name} slot needs the token its position resolves, so reading it "
                    + "from an address alone is refused.");
                return default!;

            default:
                return Unsafe.ReadUnaligned<T>((void*)Address);
        }
    }

    // The token is an ops table for a container view and the FProperty for an optional, resolved once at bind.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static T Read<T>(nint Address, nint Token)
    {
        switch (ElementKind<T>.Kind)
        {
            case EElementKind.Vector:
            case EElementKind.Map:
                return ElementKind<T>.MakeContainerView!(Address, Token);

            // Native hands back a null payload for an unset optional, which is the null a nullable wants.
            case EElementKind.Optional:
                return ElementKind<T>.MakeOptional!(Native.OptionalValueAt(Address, Token));

            default:
                return Read<T>(Address);
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Write<T>(nint Address, nint Token, T Value)
    {
        switch (ElementKind<T>.Kind)
        {
            // A view aliases the slot, so the callee's edits are already there and there is nothing to assign.
            case EElementKind.Vector:
            case EElementKind.Map:
                return;

            case EElementKind.Optional:
            {
                // Long-aligned, since the setter reads the payload back through its own type.
                long* Scratch = stackalloc long[(ElementKind<T>.OptionalPayloadSize + 7) / 8];
                if (ElementKind<T>.StageOptional!(Value, (nint)Scratch))
                {
                    Native.OptionalSetValueAt(Address, Token, (IntPtr)Scratch);
                }
                else
                {
                    Native.OptionalResetAt(Address, Token);
                }
                return;
            }

            default:
                Write(Address, Value);
                return;
        }
    }

    // Overwrites the element at Address, doing whatever its kind needs to keep the native side consistent.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Write<T>(nint Address, T Value)
    {
        switch (ElementKind<T>.Kind)
        {
            case EElementKind.String:
            {
                FString Text = Unsafe.As<T, FString>(ref Value);
                Native.StringAssign(Address, Text.ToString());
                break;
            }
            case EElementKind.ObjectRef:
            {
                // Through the native assignment, which keeps the reference count a byte write would corrupt.
                nint Target = Unsafe.As<T, nint>(ref Value);
                Native.SetObjectPtr(Address, Target);
                break;
            }
            case EElementKind.StructView:
            {
                // Copied through the reflected struct's own ops, so a heap member is duplicated rather than aliased.
                NativeRef Source = Unsafe.As<T, NativeRef>(ref Value);
                if (!Source.IsNull)
                {
                    Native.StructAssign(Address, Source.Address, ElementKind<T>.NativeName);
                }
                break;
            }
            case EElementKind.SoftRef:
                Native.SoftPathSet(Address, ElementKind<T>.SoftReferencePath!(Value));
                break;

            case EElementKind.ManagedString:
                Native.StringAssign(Address, Unsafe.As<T, string>(ref Value) ?? string.Empty);
                break;

            case EElementKind.Bool:
                Unsafe.WriteUnaligned((void*)Address, (byte)(Unsafe.As<T, bool>(ref Value) ? 1 : 0));
                break;

            case EElementKind.ObjectWrapper:
                Native.SetObjectPtr(Address, (Value as NativeObject)?.Handle ?? IntPtr.Zero);
                break;

            case EElementKind.ScriptStruct:
                ElementKind<T>.WriteScriptStruct!(Address, Value);
                break;

            case EElementKind.SlotView:
                Debug.LogError($"A {typeof(T).Name} names the slot it views, so there is no value to assign "
                    + "back over it.");
                break;

            case EElementKind.Optional:
            case EElementKind.Vector:
            case EElementKind.Map:
                Debug.LogError($"A {typeof(T).Name} slot needs the token its position resolves, so writing it "
                    + "to an address alone is refused.");
                break;

            default:
                Unsafe.WriteUnaligned((void*)Address, Value);
                break;
        }
    }

    internal static T Detach<T>(T Value)
    {
        if (ElementKind<T>.Kind == EElementKind.String)
        {
            FString Text = Unsafe.As<T, FString>(ref Value);
            if (Text.IsBound)
            {
                FString Owned = new FString(Text.ToString());
                return Unsafe.As<FString, T>(ref Owned);
            }
        }
        return Value;
    }

    internal static nint BoundAddress<T>(T Value)
    {
        switch (ElementKind<T>.Kind)
        {
            case EElementKind.String:
                return Unsafe.As<T, FString>(ref Value).Address;
            case EElementKind.StructView:
            {
                NativeRef View = Unsafe.As<T, NativeRef>(ref Value);
                return View.IsNull ? 0 : View.Address;
            }
            default:
                return 0;
        }
    }

    // Equality for IndexOf and Contains, strings by characters and everything else by the default comparer.
    public static bool Equals<T>(T Left, T Right)
    {
        if (ElementKind<T>.Kind == EElementKind.String)
        {
            FString A = Unsafe.As<T, FString>(ref Left);
            FString B = Unsafe.As<T, FString>(ref Right);
            return A.Equals(B);
        }
        // Same element means same slot, since a container element has no property handle to reach Identical.
        if (ElementKind<T>.Kind == EElementKind.StructView)
        {
            return Unsafe.As<T, NativeRef>(ref Left).Equals(Unsafe.As<T, NativeRef>(ref Right));
        }
        return System.Collections.Generic.EqualityComparer<T>.Default.Equals(Left, Right);
    }
}
