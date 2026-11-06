using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using LuminaSharp;

namespace Lumina;

/// <summary>
/// A writable <see cref="IList{T}"/> view over a native <c>TVector&lt;T&gt;</c>, source-agnostic across
/// reflected properties, function returns, or any vector instance. Reads decode the header in place (no
/// crossing); Add/RemoveAt/Clear/Insert call the ops table so native reallocs through the owning allocator.
/// As with <see cref="List{T}"/>, a mutation invalidates any earlier span, handle or enumerator; the view
/// does not own the storage.
///
/// <para>The element may be a plain value, an <see cref="FString"/>, or a <see cref="TStrongObjectPtr{T}"/> --
/// which is why there is one view type and not three. What differs per element is only how a slot is read and
/// written, and that is <c>ElementMarshal</c>'s job; this file has no per-kind knowledge, the same way
/// <c>FScriptArrayElementDesc</c> natively has none (it dispatches through the element's own FProperty).</para>
///
/// <para>The native element stride comes from the ops table, NOT from <c>Unsafe.SizeOf&lt;T&gt;()</c>. They
/// are equal only for a blittable element: an FString slot is 24 native bytes behind an 8-byte managed
/// handle.</para>
/// </summary>
public readonly unsafe struct TVector<T> : IList<T>
{
    private readonly nint Vector;   // TVector<T> instance (mpBegin@0, mpEnd@8)
    private readonly nint Ops;      // FVectorOps for T

    /// <summary>Views an existing native TVector&lt;T&gt; through its ops table. Public because the accessors
    /// ScriptPropertyRewriter emits for a user script are compiled into the user's own assembly.</summary>
    public TVector(nint vector, nint ops)
    {
        Vector = vector;
        Ops = ops;
    }

    private VectorOps* OpsPtr => (VectorOps*)Ops;

    /// <summary>False when the property this came from did not resolve, so the view has no storage.</summary>
    public bool IsValid
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => Vector != 0 && Ops != 0;
    }

    private int Stride
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => IsValid ? (int)OpsPtr->ElementSize : 0;
    }

    // Read in place through the offsets the ops table carries, so counting never leaves managed code.
    public int Count
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get
        {
            if (!IsValid)
            {
                return 0;
            }
            uint Raw = *(uint*)((byte*)Vector + OpsPtr->CountOffset);
            uint Divisor = OpsPtr->CountDivisor;
            // A native vector counts elements, so its divisor is one and the divide is skipped.
            return (int)(Divisor == 1u ? Raw : Raw / Divisor);
        }
    }

    private byte* Data
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => *(byte**)((byte*)Vector + OpsPtr->DataOffset);
    }

    public bool IsReadOnly => false;

    /// <summary>The address of element <paramref name="index"/> in native storage.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private nint ElementAt(int index)
    {
        if ((uint)index >= (uint)Count)
        {
            ThrowIndexOutOfRange(index);
        }
        return UncheckedElementAt(index);
    }

    [System.Diagnostics.CodeAnalysis.DoesNotReturn]
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void ThrowIndexOutOfRange(int index)
    {
        throw new ArgumentOutOfRangeException(nameof(index), index, null);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private nint UncheckedElementAt(int index)
    {
        return (nint)Data + index * (nint)Stride;
    }

    /// <summary>The native storage as a writable span, for in-place edits of a blittable element.
    /// <para>Not available for a marshalled element: a span of FString handles or object slots would let a
    /// plain assignment copy bytes, which is the aliasing and refcount corruption the marshal exists to
    /// prevent. Use <see cref="Get"/> / <see cref="Set"/> there.</para></summary>
    public Span<T> AsSpan()
    {
        ThrowIfMarshalled(nameof(AsSpan));
        int Length = Count;
        return Length == 0 ? Span<T>.Empty : new Span<T>(Data, Length);
    }

    /// <summary>
    /// The element by reference, so <c>List[0] = x</c> works even when the list came from a property. A
    /// by-value indexer setter would not: assigning through a property that returns a struct is CS1612,
    /// because the setter would run on a temporary. Returning a ref sidesteps that, and it is accurate
    /// anyway -- the element lives in native memory.
    /// <para>A marshalled element reads through a held copy, so LUM0201 sends writes to <see cref="Set"/> instead.</para>
    /// </summary>
    public ref T this[int index]
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get
        {
            if (ElementKind<T>.IsMarshalled)
            {
                return ref HeldElement<T>.Hold(Get(index));
            }
            return ref Unsafe.AsRef<T>((void*)ElementAt(index));
        }
    }

    T IList<T>.this[int index]
    {
        get => Get(index);
        set => Set(index, value);
    }

    /// <summary>Reads an element. Works for every element type.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public T Get(int index) => ElementMarshal.Read<T>(ElementAt(index));

    /// <summary>Overwrites an element, doing whatever its type requires -- a native assignment for a string,
    /// a refcounted assignment for an object reference, a byte write for a plain value.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Set(int index, T value) => ElementMarshal.Write(ElementAt(index), value);

    public void Add(T item)
    {
        if (!IsValid)
        {
            return;
        }
        if (ElementKind<T>.IsMarshalled)
        {
            // A handle read out of a container still points into it, so native copies it before growing can free it.
            nint Source = ElementMarshal.BoundAddress(item);
            OpsPtr->Add((void*)Vector, (void*)Source);
            if (Source == 0)
            {
                ElementMarshal.Write(ElementAt(Count - 1), item);
            }
            return;
        }
        OpsPtr->Add((void*)Vector, Unsafe.AsPointer(ref item));
    }

    public void Clear()
    {
        if (IsValid)
        {
            OpsPtr->Reset((void*)Vector);
        }
    }

    // Grows with default-constructed elements or truncates, in one crossing rather than one per element.
    public void Resize(int Size)
    {
        if (IsValid && Size >= 0)
        {
            OpsPtr->SetNum((void*)Vector, Size);
        }
    }

    public void Reserve(int Size)
    {
        if (IsValid && Size > 0)
        {
            OpsPtr->Reserve((void*)Vector, Size);
        }
    }

    // Replaces the contents with Values in one resize and one copy, for bulk data such as a terrain heightmap.
    public void Assign(ReadOnlySpan<T> Values)
    {
        ThrowIfMarshalled(nameof(Assign));
        Resize(Values.Length);
        Values.CopyTo(AsSpan());
    }

    public void RemoveAt(int index)
    {
        if ((uint)index >= (uint)Count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }
        OpsPtr->RemoveAt((void*)Vector, index);
    }

    public void Insert(int index, T item)
    {
        if (!IsValid || (uint)index > (uint)Count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }
        // The appended copy is swapped down into place, so nothing reads an element the append may have moved.
        Add(item);
        for (int i = Count - 1; i > index; --i)
        {
            OpsPtr->Swap((void*)Vector, i, i - 1);
        }
    }

    public int IndexOf(T item)
    {
        int Length = Count;
        for (int i = 0; i < Length; ++i)
        {
            if (ElementMarshal.Equals(Get(i), item))
            {
                return i;
            }
        }
        return -1;
    }

    public bool Contains(T item) => IndexOf(item) >= 0;

    public bool Remove(T item)
    {
        int Index = IndexOf(item);
        if (Index < 0)
        {
            return false;
        }
        RemoveAt(Index);
        return true;
    }

    public void CopyTo(T[] array, int arrayIndex)
    {
        int Length = Count;
        for (int i = 0; i < Length; ++i)
        {
            array[arrayIndex + i] = Get(i);
        }
    }

    private void ThrowIfMarshalled(string What)
    {
        if (ElementKind<T>.IsMarshalled)
        {
            throw new NotSupportedException(
                $"TVector<{typeof(T).Name}> does not support {What}: the element's managed value is not its "
                + "native bytes, so a reference into storage would let an assignment copy them raw. "
                + "Use Get(index) and Set(index, value).");
        }
    }

    public Enumerator GetEnumerator() => new Enumerator(this);

    IEnumerator<T> IEnumerable<T>.GetEnumerator() => GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>Allocation-free struct enumerator. Re-reads the count each step (tolerates a moved buffer);
    /// like <see cref="List{T}"/>, structural mutation mid-iteration is undefined.</summary>
    public struct Enumerator : IEnumerator<T>
    {
        private readonly TVector<T> List;
        private readonly int Count;
        private int Index;

        // Counted once, since a mutation invalidates the enumerator the same as List<T>'s.
        internal Enumerator(TVector<T> list)
        {
            List = list;
            Count = list.Count;
            Index = -1;
        }

        public T Current
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => ElementMarshal.Read<T>(List.UncheckedElementAt(Index));
        }

        object? IEnumerator.Current => Current;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public bool MoveNext() => ++Index < Count;

        public void Reset() => Index = -1;

        public void Dispose() { }
    }
}
