using System;
using System.Collections;
using System.Collections.Generic;

using LuminaSharp;

namespace Lumina;

/// <summary>
/// A writable <see cref="IDictionary{K,V}"/> view over a native reflected map, the associative counterpart of
/// <see cref="TVector{T}"/>. Every operation goes through the map's type-erased op table, so one view works
/// for a compile-time <c>THashMap&lt;K,V&gt;</c> and for the runtime map a script property is stored in.
///
/// It is a VIEW: the storage belongs to the native object, and the view does not own it. As with
/// <see cref="Dictionary{K,V}"/>, a mutation invalidates any enumerator taken before it.
///
/// Iteration order is the native map's, stable between mutations but not otherwise meaningful.
/// </summary>
public readonly unsafe struct THashMap<K, V> : IDictionary<K, V>
    where K : unmanaged
    where V : unmanaged
{
    private readonly nint Map;   // the map instance (FScriptDynamicMap, or a THashMap<K,V>)
    private readonly nint Ops;   // FMapOps for (K,V)

    /// <summary>Views an existing native map through its ops table. Public because the accessors
    /// ScriptPropertyRewriter emits for a user script are compiled into the user's own assembly.</summary>
    public THashMap(nint map, nint ops)
    {
        Map = map;
        Ops = ops;
    }

    private MapOps* OpsPtr => (MapOps*)Ops;

    /// <summary>False when the property this came from did not resolve, so the view has no storage.</summary>
    public bool IsValid => Map != 0 && Ops != 0;

    public int Count => IsValid ? (int)OpsPtr->Size((void*)Map) : 0;

    public bool IsReadOnly => false;

    /// <summary>Reads a value, throwing when the key is absent (as <see cref="Dictionary{K,V}"/> does).
    /// <para>To WRITE, call <see cref="Set"/>. <c>Map[key] = value</c> does not compile when the map came
    /// from a property: assigning through a property that returns a struct is CS1612, because the setter
    /// would run on a temporary. A list can return its element by ref to sidestep that; a map cannot,
    /// because a ref indexer would have to insert on a miss and so would turn a READ of an absent key into
    /// a silent insert.</para></summary>
    public V this[K key]
    {
        get => TryGetValue(key, out V Value)
            ? Value
            : throw new KeyNotFoundException($"The key '{key}' is not present in the map.");
        set => Set(key, value);
    }

    /// <summary>Inserts or overwrites. Use this rather than <c>Map[key] = value</c> when the map came from a
    /// property; see the indexer.</summary>
    public void Set(K key, V value)
    {
        if (IsValid)
        {
            OpsPtr->Insert((void*)Map, &key, &value);
        }
    }

    public bool TryGetValue(K key, out V value)
    {
        if (IsValid)
        {
            void* Slot = OpsPtr->Find((void*)Map, &key);
            if (Slot != null)
            {
                value = *(V*)Slot;
                return true;
            }
        }
        value = default;
        return false;
    }

    public bool ContainsKey(K key) => IsValid && OpsPtr->Find((void*)Map, &key) != null;

    /// <summary>Adds a pair. Unlike <see cref="Dictionary{K,V}.Add"/> this overwrites an existing key rather
    /// than throwing, matching the native map's insert-or-assign.</summary>
    public void Add(K key, V value) => Set(key, value);

    public bool Remove(K key) => IsValid && OpsPtr->RemoveByKey((void*)Map, &key) != 0;

    public void Clear()
    {
        if (IsValid)
        {
            OpsPtr->Clear((void*)Map);
        }
    }

    /// <summary>Pre-sizes the map for an expected entry count.</summary>
    public void Reserve(int count)
    {
        if (IsValid && count > 0)
        {
            OpsPtr->Reserve((void*)Map, (nuint)count);
        }
    }

    public ICollection<K> Keys
    {
        get
        {
            var Result = new List<K>(Count);
            foreach (KeyValuePair<K, V> Pair in this)
            {
                Result.Add(Pair.Key);
            }
            return Result;
        }
    }

    public ICollection<V> Values
    {
        get
        {
            var Result = new List<V>(Count);
            foreach (KeyValuePair<K, V> Pair in this)
            {
                Result.Add(Pair.Value);
            }
            return Result;
        }
    }

    public void Add(KeyValuePair<K, V> item) => Add(item.Key, item.Value);

    public bool Contains(KeyValuePair<K, V> item)
    {
        return TryGetValue(item.Key, out V Found) && EqualityComparer<V>.Default.Equals(Found, item.Value);
    }

    public bool Remove(KeyValuePair<K, V> item) => Contains(item) && Remove(item.Key);

    public void CopyTo(KeyValuePair<K, V>[] array, int arrayIndex)
    {
        foreach (KeyValuePair<K, V> Pair in this)
        {
            array[arrayIndex++] = Pair;
        }
    }

    public Enumerator GetEnumerator() => new Enumerator(Map, Ops);

    IEnumerator<KeyValuePair<K, V>> IEnumerable<KeyValuePair<K, V>>.GetEnumerator() => GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>Allocation-free struct enumerator over the native iteration order, addressed by index so it
    /// tolerates the container reallocating between steps.</summary>
    public struct Enumerator : IEnumerator<KeyValuePair<K, V>>
    {
        private readonly nint Map;
        private readonly nint Ops;
        private int Index;

        internal Enumerator(nint map, nint ops)
        {
            Map = map;
            Ops = ops;
            Index = -1;
            Current = default;
        }

        public KeyValuePair<K, V> Current { get; private set; }

        object IEnumerator.Current => Current;

        public bool MoveNext()
        {
            if (Map == 0 || Ops == 0)
            {
                return false;
            }

            MapOps* Table = (MapOps*)Ops;
            ++Index;
            if ((nuint)Index >= Table->Size((void*)Map))
            {
                return false;
            }

            void* Key = null;
            void* Value = null;
            Table->At((void*)Map, (nuint)Index, &Key, &Value);
            if (Key == null || Value == null)
            {
                return false;
            }

            Current = new KeyValuePair<K, V>(*(K*)Key, *(V*)Value);
            return true;
        }

        public void Reset()
        {
            Index = -1;
            Current = default;
        }

        public void Dispose() { }
    }
}
