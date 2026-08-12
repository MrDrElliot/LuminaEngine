using System;
using System.Runtime.CompilerServices;
using Lumina;

namespace LuminaSharp;

/// <summary>How a container element is carried between native storage and C#.</summary>
internal enum EElementKind
{
    /// <summary>The managed value IS the native bytes. Read and written in place; the fast path.</summary>
    Blittable,

    /// <summary>An <c>eastl::basic_string</c>. Read by decoding in place, written through eastl assignment.</summary>
    String,

    /// <summary>A refcounted object slot. Read as a raw pointer, written through the native assignment so the
    /// reference it held is released and one is taken on the new target.</summary>
    ObjectRef,
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
        return EElementKind.Blittable;
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
        return System.Collections.Generic.EqualityComparer<T>.Default.Equals(Left, Right);
    }
}
