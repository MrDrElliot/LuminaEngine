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

    /// <summary>A refcounted object slot. Read as a raw pointer, written through the native assignment so the
    /// reference it held is released and one is taken on the new target.</summary>
    ObjectRef,

    /// <summary>An opaque reflected struct. Read as a wrapper viewing the slot, written by native struct copy.</summary>
    StructView,

    /// <summary>An asset reference. Stored natively as one FSoftObjectPath, so the path is the whole value.</summary>
    SoftRef,

    // Native writes any nonzero byte, so the read normalizes rather than reinterpreting the slot.
    Bool,

    // A managed string over the same FString storage FString names, decoded rather than handled.
    ManagedString,

    // A CObject wrapper over a TObjectPtr slot, resolved through the managed-instance cache so identity holds.
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
}

/// <summary>
/// The element kind for one <c>T</c>, resolved once.
///
/// This is what lets a single <see cref="TVector{T}"/> hold plain values, strings and object references
/// instead of needing a bespoke view type per element flavour -- mirroring what native already does, where
/// <c>FScriptArrayElementDesc</c> dispatches construct/destruct/copy through the element's own FProperty and
/// <c>FVectorOps::PushBack</c> runs a real C++ copy-construct.
///
/// A <c>static readonly</c> field on a generic type is a JIT constant once the static constructor has run, and
/// generics over value types are specialised, so the <see cref="EElementKind.Blittable"/> branch folds away
/// entirely: <c>TVector&lt;int&gt;</c> compiles to the same direct memory access it did when the constraint
/// was <c>unmanaged</c>.
/// </summary>
internal static class ElementKind<T>
{
    public static readonly EElementKind Kind = ElementKinds.Of(typeof(T));

    /// <summary>True for the kinds whose managed value is not the native bytes, so a <c>ref</c> into storage
    /// would be wrong -- either the wrong layout, or a write that skips the bookkeeping.</summary>
    public static readonly bool IsMarshalled = Kind is not (EElementKind.Blittable or EElementKind.Bool);

    public static readonly Func<IntPtr, T>? MakeObject = BindObject();

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

    public static readonly Func<string, T>? MakeAssetRef = BindAssetRef<Func<string, T>>(nameof(AssetRefMarshal.Read));

    public static readonly Func<T, string>? AssetRefPath = BindAssetRef<Func<T, string>>(nameof(AssetRefMarshal.Write));

    // Only AssetRefMarshal carries the struct constraint that makes the interface calls constrained ones.
    private static TDelegate? BindAssetRef<TDelegate>(string Method) where TDelegate : Delegate
    {
        if (Kind != EElementKind.SoftRef || !typeof(T).IsValueType)
        {
            return null;
        }

        return (TDelegate)typeof(AssetRefMarshal).GetMethod(Method)!
            .MakeGenericMethod(typeof(T))
            .CreateDelegate(typeof(TDelegate));
    }

    private static Func<nint, T>? BuildView()
    {
        if (Kind is not (EElementKind.StructView or EElementKind.SlotView))
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
    public static T Read<T>(nint Address)
    {
        switch (ElementKind<T>.Kind)
        {
            case EElementKind.String:
            {
                // Unsafe.As rather than a cast through object: the kind guarantees T is FString, and boxing
                // every element read would defeat the point of a view.
                FString Value = new FString(Address);
                return Unsafe.As<FString, T>(ref Value);
            }
            case EElementKind.ObjectRef:
                // TObjectPtr<X> is a single IntPtr, and so is the native slot, so the read is the same
                // reinterpretation a blittable element gets. Only the WRITE differs.
                return Unsafe.ReadUnaligned<T>((void*)Address);

            case EElementKind.StructView:
            case EElementKind.SlotView:
                // The wrapper points at the slot, so a mutation through it edits the array in place.
                return ElementKind<T>.MakeView!(Address);

            case EElementKind.SoftRef:
                // Casting the local to IAssetRef would mutate a box this then throws away, leaving the default.
                return ElementKind<T>.MakeAssetRef!(Native.SoftPathGet(Address));

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

    /// <summary>Overwrites the element at <paramref name="Address"/>, doing whatever its kind requires to
    /// leave the native side consistent.</summary>
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
                // Through the native assignment, which releases the reference the slot held and takes one on
                // the new target. A byte write here is the silent refcount corruption this kind exists for.
                nint Target = Unsafe.As<T, nint>(ref Value);
                Native.SetObjectPtr(Address, Target);
                break;
            }
            case EElementKind.StructView:
            {
                // Copied through the reflected struct's own ops, so a heap member is duplicated rather than aliased.
                if (Value is NativeStruct Source && Source.Handle != IntPtr.Zero)
                {
                    Native.StructAssign(Address, Source.Handle, ElementKind<T>.NativeName);
                }
                break;
            }
            case EElementKind.SoftRef:
                Native.SoftPathSet(Address, ElementKind<T>.AssetRefPath!(Value));
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

    /// <summary>Equality for IndexOf/Contains. Strings compare by characters; everything else by bits, via the
    /// default comparer.</summary>
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
            return Left is NativeStruct A2 && Right is NativeStruct B2 && A2.Handle == B2.Handle;
        }
        return System.Collections.Generic.EqualityComparer<T>.Default.Equals(Left, Right);
    }
}
