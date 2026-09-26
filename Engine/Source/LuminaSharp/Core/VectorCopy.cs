using System.Collections.Generic;
using Lumina;

namespace LuminaSharp;

// A frame return owns its slot, so a container crosses by value here where a view would have nothing to give back.
internal static class VectorCopy
{
    public static object ToList<T>(nint Address, nint Ops)
    {
        var View = new TVector<T>(Address, Ops);
        var Copy = new List<T>(View.Count);
        for (int Index = 0; Index < View.Count; ++Index)
        {
            Copy.Add(View.Get(Index));
        }
        return Copy;
    }

    public static object ToArray<T>(nint Address, nint Ops)
    {
        var View = new TVector<T>(Address, Ops);
        var Copy = new T[View.Count];
        for (int Index = 0; Index < Copy.Length; ++Index)
        {
            Copy[Index] = View.Get(Index);
        }
        return Copy;
    }

    public static void Fill<T>(nint Address, nint Ops, object? Items)
    {
        var View = new TVector<T>(Address, Ops);
        View.Clear();
        if (Items is IEnumerable<T> Source)
        {
            foreach (T Item in Source)
            {
                View.Add(Item);
            }
        }
    }
}
