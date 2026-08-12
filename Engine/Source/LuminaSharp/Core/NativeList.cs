using System;
using System.Collections;
using System.Collections.Generic;

namespace LuminaSharp;

// Mirror of native Lumina::FVectorOps (Containers/ContainerOps.h). C# calls only PushBack/RemoveAt/Clear
// (reads decode the header directly), and [Cdecl] matches the captureless lambdas on x64.
//
// This is a PREFIX of the native struct, not the whole of it: the fields up to ElementSize must stay at the
// same offsets, and native appends anything new after them (it carries container construct/destruct entries
// C# has no use for). So a native change is safe here only while it is append-only.
#pragma warning disable CS0649 // fields are populated by overlaying native memory, not managed assignment
internal unsafe struct VectorOps
{
    public delegate* unmanaged[Cdecl]<void*, nuint> Size;
    public delegate* unmanaged[Cdecl]<void*, void*> Data;
    public delegate* unmanaged[Cdecl]<void*, void*, void> PushBack;
    public delegate* unmanaged[Cdecl]<void*, nuint, void> RemoveAt;
    public delegate* unmanaged[Cdecl]<void*, void> Clear;
    public delegate* unmanaged[Cdecl]<void*, nuint, void> Resize;
    public delegate* unmanaged[Cdecl]<void*, nuint, void> Reserve;
    public delegate* unmanaged[Cdecl]<void*, nuint, nuint, void> Swap;
    public uint ElementSize;
}
#pragma warning restore CS0649

/// <summary>
/// A writable <see cref="IList{T}"/> view over a native <c>TVector&lt;T&gt;</c> (blittable element), source-agnostic
/// across reflected properties, function returns, or any vector instance. Reads decode the header in place (no
/// crossing); Add/RemoveAt/Clear/Insert call the ops table so native reallocs through the owning allocator. As with
/// <see cref="List{T}"/>, a mutation invalidates any earlier span/enumerator; the view does not own the storage.
/// </summary>
public readonly unsafe struct NativeList<T> : IList<T> where T : unmanaged
{
    private readonly nint Vector;   // TVector<T> instance (mpBegin@0, mpEnd@8)
    private readonly nint Ops;      // FVectorOps for T

    /// <summary>Views an existing native TVector&lt;T&gt; through its ops table. Public because the accessors
    /// ScriptPropertyRewriter emits for a user script are compiled into the user's own assembly.</summary>
    public NativeList(nint vector, nint ops)
    {
        Vector = vector;
        Ops = ops;
    }

    private VectorOps* OpsPtr => (VectorOps*)Ops;

    /// <summary>The native storage as a writable span. In-place element edits only; a mutation invalidates it.</summary>
    public Span<T> AsSpan() => NativeMarshal.DecodeVector<T>((byte*)Vector);

    public int Count => AsSpan().Length;

    public bool IsReadOnly => false;

    public T this[int index]
    {
        get => AsSpan()[index];
        set => AsSpan()[index] = value;
    }

    public void Add(T item)
    {
        OpsPtr->PushBack((void*)Vector, &item);
    }

    public void Clear()
    {
        OpsPtr->Clear((void*)Vector);
    }

    public void RemoveAt(int index)
    {
        if ((uint)index >= (uint)Count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }
        OpsPtr->RemoveAt((void*)Vector, (nuint)index);
    }

    public void Insert(int index, T item)
    {
        if ((uint)index > (uint)Count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }
        // Grow by one (the appended value is overwritten by the shift below), then open a gap at index.
        OpsPtr->PushBack((void*)Vector, &item);
        Span<T> Span = AsSpan();
        for (int i = Span.Length - 1; i > index; --i)
        {
            Span[i] = Span[i - 1];
        }
        Span[index] = item;
    }

    public int IndexOf(T item)
    {
        Span<T> Span = AsSpan();
        for (int i = 0; i < Span.Length; ++i)
        {
            if (EqualityComparer<T>.Default.Equals(Span[i], item))
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

    public void CopyTo(T[] array, int arrayIndex) => AsSpan().CopyTo(array.AsSpan(arrayIndex));

    public Enumerator GetEnumerator() => new Enumerator(Vector);

    IEnumerator<T> IEnumerable<T>.GetEnumerator() => GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>Allocation-free struct enumerator. Re-reads the header each step (tolerates a moved buffer); like
    /// <see cref="List{T}"/>, structural mutation mid-iteration is undefined.</summary>
    public struct Enumerator : IEnumerator<T>
    {
        private readonly nint Vector;
        private int Index;

        internal Enumerator(nint vector)
        {
            Vector = vector;
            Index = -1;
        }

        private Span<T> Span => NativeMarshal.DecodeVector<T>((byte*)Vector);

        public T Current => Span[Index];

        object IEnumerator.Current => Current;

        public bool MoveNext() => ++Index < Span.Length;

        public void Reset() => Index = -1;

        public void Dispose() { }
    }
}
