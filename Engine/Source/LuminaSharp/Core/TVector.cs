using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using LuminaSharp;

namespace Lumina;

// Writable IList view over a native TVector; a mutation invalidates earlier spans and enumerators, and the stride comes from the ops table.
public readonly unsafe struct TVector<T> : IList<T>
{
    private readonly nint Vector;   // TVector<T> instance (mpBegin@0, mpEnd@8)
    private readonly nint Ops;      // FVectorOps for T

    // Public because ScriptPropertyRewriter accessors compile into the user's own assembly.
    public TVector(nint vector, nint ops)
    {
        Vector = vector;
        Ops = ops;
    }

    private VectorOps* OpsPtr => (VectorOps*)Ops;

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

    // Blittable elements only, since a span of marshalled slots would let a plain assignment corrupt refcounts.
    public Span<T> AsSpan()
    {
        ThrowIfMarshalled(nameof(AsSpan));
        int Length = Count;
        return Length == 0 ? Span<T>.Empty : new Span<T>(Data, Length);
    }

    // Returns by ref so assigning through a struct-returning property avoids CS1612; marshalled elements write through Set.
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

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public T Get(int index) => ElementMarshal.Read<T>(ElementAt(index));

    // Assigns natively for a string, refcounted for an object reference, and bytewise for a plain value.
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

    // Re-reads the count each step; structural mutation mid-iteration is undefined.
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
