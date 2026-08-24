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
    public static readonly EElementKind Kind = Resolve();

    /// <summary>True for the kinds whose managed value is not the native bytes, so a <c>ref</c> into storage
    /// would be wrong -- either the wrong layout, or a write that skips the bookkeeping.</summary>
    public static readonly bool IsMarshalled = Kind != EElementKind.Blittable;

    private static EElementKind Resolve()
    {
        Type Element = typeof(T);
        if (Element == typeof(FString))
        {
            return EElementKind.String;
        }
        // A TObjectPtr<X> element is pointer-sized and READS like a blittable one, but assigning it by bytes
        // would store the pointer without taking a reference and without releasing the one it replaced.
        if (Element.IsGenericType && Element.GetGenericTypeDefinition() == typeof(TObjectPtr<>))
        {
            return EElementKind.ObjectRef;
        }
        // Recognised by the interface rather than by name, so a new asset-reference type needs no change here.
        if (typeof(IAssetRef).IsAssignableFrom(Element))
        {
            return EElementKind.SoftRef;
        }
        // A generated wrapper views native memory rather than holding it, so both ends go through a real copy.
        if (typeof(NativeStruct).IsAssignableFrom(Element))
        {
            return EElementKind.StructView;
        }
        return EElementKind.Blittable;
    }

    /// <summary>Builds the slot view once per T, from the same (IntPtr) ctor Wrapper uses.</summary>
    public static readonly Func<nint, T>? MakeView = BuildView();

    /// <summary>The reflected struct name StructAssign resolves; empty for every other kind.</summary>
    public static readonly string NativeName = Kind == EElementKind.StructView ? NativeTypeName.Of<T>() : string.Empty;

    private static Func<nint, T>? BuildView()
    {
        if (Kind != EElementKind.StructView)
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
internal static unsafe class ElementMarshal
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
                // The wrapper points at the slot, so a mutation through it edits the array in place.
                return ElementKind<T>.MakeView!(Address);

            case EElementKind.SoftRef:
            {
                T Value = default!;
                ((IAssetRef)Value!).SetFromPath(Native.SoftPathGet(Address));
                return Value;
            }

            default:
                return Unsafe.ReadUnaligned<T>((void*)Address);
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
            {
                Native.SoftPathSet(Address, ((IAssetRef)Value!).GetPath() ?? "");
                break;
            }
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
