using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using LuminaSharp;

namespace Lumina;

// Writable IDictionary view over a native reflected map through its type-erased op table; a mutation invalidates earlier enumerators.
public readonly unsafe struct THashMap<K, V> : IDictionary<K, V>
{
    private readonly nint Map;   // the map instance (FScriptDynamicMap, or a THashMap<K,V>)
    private readonly nint Ops;   // FMapOps for (K,V)

    // Public because ScriptPropertyRewriter accessors compile into the user's own assembly.
    public THashMap(nint map, nint ops)
    {
        Map = map;
        Ops = ops;
    }

    private MapOps* OpsPtr => (MapOps*)Ops;

    public bool IsValid => Map != 0 && Ops != 0;

    public int Count => IsValid ? OpsPtr->Num((void*)Map) : 0;

    public bool IsReadOnly => false;

    // Read-only, since a ref indexer would insert on a miss; write with Set to avoid CS1612 on a property-returned map.
    public V this[K key]
    {
        get => TryGetValue(key, out V Value)
            ? Value
            : throw new KeyNotFoundException($"The key '{key}' is not present in the map.");
        set => Set(key, value);
    }

    public void Set(K key, V value)
    {
        if (!IsValid)
        {
            return;
        }

        if (!ElementKind<V>.IsMarshalled)
        {
            InsertKey(key, Unsafe.AsPointer(ref value));
            return;
        }

        // A handle read out of a container is copied by native before inserting can rehash it away.
        nint Source = ElementMarshal.BoundAddress(value);
        void* Slot = InsertKey(key, (void*)Source);
        if (Slot != null && Source == 0)
        {
            ElementMarshal.Write<V>((nint)Slot, value);
        }
    }

    // A marshalled key reaches native as a constructed value of its own native width, torn down after the call.
    private void* InsertKey(K key, void* value)
    {
        if (!ElementKind<K>.IsMarshalled)
        {
            return OpsPtr->Add((void*)Map, Unsafe.AsPointer(ref key), value);
        }

        byte* Scratch = stackalloc byte[(int)OpsPtr->KeySize];
        OpsPtr->ConstructKey((void*)Map, Scratch);
        try
        {
            ElementMarshal.Write<K>((nint)Scratch, key);
            return OpsPtr->Add((void*)Map, Scratch, value);
        }
        finally
        {
            OpsPtr->DestructKey((void*)Map, Scratch);
        }
    }

    private void* FindKey(K key)
    {
        if (!ElementKind<K>.IsMarshalled)
        {
            return OpsPtr->Find((void*)Map, Unsafe.AsPointer(ref key));
        }

        byte* Scratch = stackalloc byte[(int)OpsPtr->KeySize];
        OpsPtr->ConstructKey((void*)Map, Scratch);
        try
        {
            ElementMarshal.Write<K>((nint)Scratch, key);
            return OpsPtr->Find((void*)Map, Scratch);
        }
        finally
        {
            OpsPtr->DestructKey((void*)Map, Scratch);
        }
    }

    private bool RemoveKey(K key)
    {
        if (!ElementKind<K>.IsMarshalled)
        {
            return OpsPtr->Remove((void*)Map, Unsafe.AsPointer(ref key)) != 0;
        }

        byte* Scratch = stackalloc byte[(int)OpsPtr->KeySize];
        OpsPtr->ConstructKey((void*)Map, Scratch);
        try
        {
            ElementMarshal.Write<K>((nint)Scratch, key);
            return OpsPtr->Remove((void*)Map, Scratch) != 0;
        }
        finally
        {
            OpsPtr->DestructKey((void*)Map, Scratch);
        }
    }

    public bool TryGetValue(K key, out V value)
    {
        if (IsValid)
        {
            void* Slot = FindKey(key);
            if (Slot != null)
            {
                value = ElementMarshal.Read<V>((nint)Slot);
                return true;
            }
        }
        value = default!;
        return false;
    }

    public bool ContainsKey(K key) => IsValid && FindKey(key) != null;

    // Overwrites an existing key instead of throwing, matching the native insert-or-assign.
    public void Add(K key, V value) => Set(key, value);

    public bool Remove(K key) => IsValid && RemoveKey(key);

    public void Clear()
    {
        if (IsValid)
        {
            OpsPtr->Reset((void*)Map);
        }
    }

    public void Reserve(int count)
    {
        if (IsValid && count > 0)
        {
            OpsPtr->Reserve((void*)Map, count);
        }
    }

    public KeyView Keys => new(this);

    public ValueView Values => new(this);

    // The interface wants collections it can hold onto, so only these copy.
    ICollection<K> IDictionary<K, V>.Keys
    {
        get
        {
            var Result = new List<K>(Count);
            foreach (K Key in Keys)
            {
                Result.Add(Key);
            }
            return Result;
        }
    }

    ICollection<V> IDictionary<K, V>.Values
    {
        get
        {
            var Result = new List<V>(Count);
            foreach (V Value in Values)
            {
                Result.Add(Value);
            }
            return Result;
        }
    }

    public readonly struct KeyView
    {
        private readonly THashMap<K, V> Map;

        internal KeyView(THashMap<K, V> map) => Map = map;

        public int Count => Map.Count;

        public KeyEnumerator GetEnumerator() => new(Map.GetEnumerator());
    }

    public readonly struct ValueView
    {
        private readonly THashMap<K, V> Map;

        internal ValueView(THashMap<K, V> map) => Map = map;

        public int Count => Map.Count;

        public ValueEnumerator GetEnumerator() => new(Map.GetEnumerator());
    }

    public struct KeyEnumerator
    {
        private Enumerator Pairs;

        internal KeyEnumerator(Enumerator pairs) => Pairs = pairs;

        public K Current => Pairs.CurrentKey;

        public bool MoveNext() => Pairs.MoveNext();
    }

    public struct ValueEnumerator
    {
        private Enumerator Pairs;

        internal ValueEnumerator(Enumerator pairs) => Pairs = pairs;

        public V Current => Pairs.CurrentValue;

        public bool MoveNext() => Pairs.MoveNext();
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

    // Resumes from a native cursor, so a walk is linear and stays in bounds if the map rehashes between steps.
    public struct Enumerator : IEnumerator<KeyValuePair<K, V>>
    {
        private readonly nint Map;
        private readonly nint Ops;
        private int Cursor;
        private nint Key;
        private nint Value;

        internal Enumerator(nint map, nint ops)
        {
            Map = map;
            Ops = ops;
            Cursor = 0;
            Key = 0;
            Value = 0;
        }

        public KeyValuePair<K, V> Current => new(CurrentKey, CurrentValue);

        internal K CurrentKey => ElementMarshal.Read<K>(Key);

        internal V CurrentValue => ElementMarshal.Read<V>(Value);

        object IEnumerator.Current => Current;

        public bool MoveNext()
        {
            if (Map == 0 || Ops == 0)
            {
                return false;
            }

            int Next = Cursor;
            void* FoundKey = null;
            void* FoundValue = null;
            bool bFound = ((MapOps*)Ops)->Next((void*)Map, &Next, &FoundKey, &FoundValue) != 0;
            Cursor = Next;
            Key = (nint)FoundKey;
            Value = (nint)FoundValue;
            return bFound;
        }

        public void Reset()
        {
            Cursor = 0;
            Key = 0;
            Value = 0;
        }

        public void Dispose() { }
    }
}
