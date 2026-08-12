#pragma once

#include "Containers/Array.h"

namespace Lumina
{
    // Type-erased operation table for a TVector<T>: a vtable of pure function pointers that operate on a
    // TVector<T> instance (the vector pointer itself, NOT the owning object) without knowing T. One static
    // table per element type, produced by GetVectorOps<T>(). This is the single source of truth for "how to
    // operate on a reflected vector," shared by:
    //   - FArrayProperty (Core reflection): serialization / net / copy / editor property table.
    //   - Lumina.TVector<T> (C#): reads decode the EASTL header in place; mutators call these fn-ptrs.
    //
    // It is deliberately decoupled from reflection: any TVector<T> -- a reflected member, a function return, a
    // plain local -- is operated on the same way. The field order/layout MUST match LuminaSharp.VectorOps
    // (Core/ContainerOps.cs); the C# side reads PushBack/RemoveAt/Clear at fixed offsets. Captureless lambdas
    // convert to plain function pointers whose calling convention matches C#'s delegate* unmanaged[Cdecl].
    struct FVectorOps
    {
        SIZE_T (*Size)(const void* Vector);
        void*  (*Data)(void* Vector);
        void   (*PushBack)(void* Vector, const void* Element); // null Element => default-construct (emplace_back)
        void   (*RemoveAt)(void* Vector, SIZE_T Index);
        void   (*Clear)(void* Vector);
        void   (*Resize)(void* Vector, SIZE_T Size);
        void   (*Reserve)(void* Vector, SIZE_T Size);
        void   (*Swap)(void* Vector, SIZE_T LHS, SIZE_T RHS);
        uint32 ElementSize;
        // Bring the CONTAINER itself up / tear it down in caller-owned raw memory (as opposed to the element
        // ops above, which all assume a live container). This is what lets FArrayProperty implement
        // ConstructValue/DestructValue without knowing whether it is looking at a TVector<T> or a script
        // runtime array. New fields go HERE, after ElementSize: LuminaSharp.VectorOps reads the earlier
        // fields at fixed offsets, so appending is safe and inserting is not.
        //
        // Context is handed back to both, opaque to everything in between. A compile-time TVector<T> needs
        // none (the type is baked into the lambda); a runtime script array needs it to reach the element
        // description it must wire into the freshly-constructed container.
        void   (*ConstructContainer)(void* Vector, const void* Context);
        void   (*DestructContainer)(void* Vector, const void* Context);
        const void* ContainerContext;
    };

    template <typename T>
    const FVectorOps* GetVectorOps()
    {
        static const FVectorOps Ops =
        {
            [](const void* V) -> SIZE_T { return static_cast<const TVector<T>*>(V)->size(); },
            [](void* V) -> void* { return static_cast<TVector<T>*>(V)->data(); },
            [](void* V, const void* E) { TVector<T>* Vec = static_cast<TVector<T>*>(V); if (E) { Vec->push_back(*static_cast<const T*>(E)); } else { Vec->emplace_back(); } },
            [](void* V, SIZE_T I) { TVector<T>* Vec = static_cast<TVector<T>*>(V); Vec->erase(Vec->begin() + I); },
            [](void* V) { static_cast<TVector<T>*>(V)->clear(); },
            [](void* V, SIZE_T N) { static_cast<TVector<T>*>(V)->resize(N); },
            [](void* V, SIZE_T N) { static_cast<TVector<T>*>(V)->reserve(N); },
            [](void* V, SIZE_T A, SIZE_T B) { TVector<T>* Vec = static_cast<TVector<T>*>(V); T Tmp = (*Vec)[A]; (*Vec)[A] = (*Vec)[B]; (*Vec)[B] = Tmp; },
            static_cast<uint32>(sizeof(T)),
            [](void* V, const void*) { new (V) TVector<T>(); },
            [](void* V, const void*) { static_cast<TVector<T>*>(V)->~TVector<T>(); },
            nullptr,
        };
        return &Ops;
    }

    // Type-erased op table for a THashMap<K,V>, the associative analogue of FVectorOps; one static table per
    // (K,V) via GetMapOps<K,V>(). Not index-addressable, so no Data/GetAt/Swap/Resize. Each captureless lambda
    // casts void* back to concrete K/V, so hashing/equality run on the real types.
    struct FMapOps
    {
        SIZE_T (*Size)(const void* Map);
        // Insert-or-assign; ValuePtr null => value default-constructed. Returns the stored value slot.
        void*  (*Insert)(void* Map, const void* KeyPtr, const void* ValuePtr);
        void*  (*Find)(void* Map, const void* KeyPtr);          // stored value for KeyPtr, or null
        bool   (*RemoveByKey)(void* Map, const void* KeyPtr);
        void   (*Clear)(void* Map);
        void   (*Reserve)(void* Map, SIZE_T Count);
        void   (*ForEach)(const void* Map, void (*Visitor)(const void* KeyPtr, void* ValuePtr, void* UserData), void* UserData);
        // Placement construct/destruct a scratch key (deserialize path). Map is passed so a type-erased runtime
        // map can reach its key element description; the compile-time map ignores it.
        void   (*ConstructKey)(void* Map, void* Dst);
        void   (*DestructKey)(void* Map, void* Dst);
        // Key/value slot for the pair at iteration index (null out-params if out of range). Iteration order is
        // stable between mutations, so an editor can address entries by index. O(index) here, O(1) for the script map.
        void   (*At)(void* Map, SIZE_T Index, const void** OutKey, void** OutValue);
        uint32 KeySize;
        uint32 ValueSize;
        // The map counterpart of FVectorOps::Construct/DestructContainer, Context and all: brings the MAP
        // itself up / tears it down in caller-owned raw memory, so FMapProperty can implement
        // ConstructValue/DestructValue without knowing whether it holds a THashMap<K,V> or a script map.
        void   (*ConstructContainer)(void* Map, const void* Context);
        void   (*DestructContainer)(void* Map, const void* Context);
        const void* ContainerContext;
    };

    template <typename K, typename V>
    const FMapOps* GetMapOps()
    {
        using MapT = THashMap<K, V>;
        static const FMapOps Ops =
        {
            [](const void* M) -> SIZE_T { return static_cast<const MapT*>(M)->size(); },
            [](void* M, const void* KP, const void* VP) -> void*
            {
                MapT* Map = static_cast<MapT*>(M);
                V& Slot = (*Map)[*static_cast<const K*>(KP)];  // operator[] default-constructs on insert
                if (VP) { Slot = *static_cast<const V*>(VP); }
                return &Slot;
            },
            [](void* M, const void* KP) -> void*
            {
                MapT* Map = static_cast<MapT*>(M);
                auto It = Map->find(*static_cast<const K*>(KP));
                return It != Map->end() ? &It->second : nullptr;
            },
            [](void* M, const void* KP) -> bool
            {
                return static_cast<MapT*>(M)->erase(*static_cast<const K*>(KP)) != 0;
            },
            [](void* M) { static_cast<MapT*>(M)->clear(); },
            [](void* M, SIZE_T N) { static_cast<MapT*>(M)->reserve(N); },
            [](const void* M, void (*Visitor)(const void*, void*, void*), void* UserData)
            {
                // value_type is pair<const K, V>; hand out &first (key) and a mutable &second (value). ForEach is
                // only used on the read side (serialize-out / copy-out / compare), so the mutable value is never
                // written through here -- the const_cast just erases the pair's constness for the void* signature.
                for (const auto& Pair : *static_cast<const MapT*>(M))
                {
                    Visitor(&Pair.first, const_cast<V*>(&Pair.second), UserData);
                }
            },
            [](void*, void* Dst) { new (Dst) K(); },
            [](void*, void* Dst) { static_cast<K*>(Dst)->~K(); },
            [](void* M, SIZE_T Index, const void** OutKey, void** OutValue)
            {
                MapT* Map = static_cast<MapT*>(M);
                SIZE_T i = 0;
                for (auto& Pair : *Map)
                {
                    if (i++ == Index)
                    {
                        if (OutKey)   { *OutKey = &Pair.first; }
                        if (OutValue) { *OutValue = &Pair.second; }
                        return;
                    }
                }
                if (OutKey)   { *OutKey = nullptr; }
                if (OutValue) { *OutValue = nullptr; }
            },
            static_cast<uint32>(sizeof(K)),
            static_cast<uint32>(sizeof(V)),
            [](void* M, const void*) { new (M) MapT(); },
            [](void* M, const void*) { static_cast<MapT*>(M)->~MapT(); },
            nullptr,
        };
        return &Ops;
    }
}
