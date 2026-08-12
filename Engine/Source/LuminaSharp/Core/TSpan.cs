using System;
using System.Collections;
using System.Collections.Generic;

using LuminaSharp;

namespace Lumina;

/// <summary>A read-only view over a native TVector&lt;T&gt; with a non-blittable element (string / opaque-struct
/// wrapper). Holds the container context + a static projector function pointer, so a property read allocates no
/// closure. Snapshots the count; does not own the storage, so don't retain it past the owner's lifetime.</summary>
public readonly unsafe struct TSpan<T> : IReadOnlyList<T>
{
    private readonly int Length;
    private readonly nint Context;
    private readonly delegate*<nint, int, T> Projector;

    public TSpan(int Count, nint Context, delegate*<nint, int, T> Projector)
    {
        Length = Count < 0 ? 0 : Count;
        this.Context = Context;
        this.Projector = Projector;
    }

    public int Count => Length;

    public T this[int Index]
    {
        get
        {
            if ((uint)Index >= (uint)Length)
            {
                throw new ArgumentOutOfRangeException(nameof(Index));
            }
            return Projector(Context, Index);
        }
    }

    public Enumerator GetEnumerator() => new(this);

    IEnumerator<T> IEnumerable<T>.GetEnumerator() => GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>Allocation-free struct enumerator.</summary>
    public struct Enumerator : IEnumerator<T>
    {
        private readonly TSpan<T> List;
        private int Index;

        internal Enumerator(TSpan<T> list)
        {
            List = list;
            Index = -1;
        }

        public T Current => List.Projector(List.Context, Index);

        object IEnumerator.Current => Current!;

        public bool MoveNext() => ++Index < List.Length;

        public void Reset() => Index = -1;

        public void Dispose() { }
    }
}
