#pragma once

#include "Containers/HashTable.h"
#include "Containers/Optional.h"
#include "Memory/Construct.h"
#include "Containers/Vector.h"

namespace Lumina
{
    // Type-erased operation table for a TVector<T>: a vtable of pure function pointers that operate on a
    // TVector<T> instance (the vector pointer itself, NOT the owning object) without knowing T. One static
    // table per element type, produced by GetVectorOps<T>(). This is the single source of truth for "how to
    // operate on a reflected vector," shared by:
    //   - FArrayProperty (Core reflection): serialization / net / copy / editor property table.
    //   - Lumina.TVector<T> (C#): reads decode the vector header in place; mutators call these fn-ptrs.
    //
    // It is deliberately decoupled from reflection: any TVector<T> -- a reflected member, a function return, a
    // plain local -- is operated on the same way. The field order/layout MUST match LuminaSharp.VectorOps
    // (Core/ContainerOps.cs); the C# side reads Add/RemoveAt/Reset at fixed offsets. Captureless lambdas
    // convert to plain function pointers whose calling convention matches C#'s delegate* unmanaged[Cdecl].
    struct FVectorOps
    {
        int32  (*Num)(const void* Vector);
        void*  (*GetData)(void* Vector);
        void   (*Add)(void* Vector, const void* Element); // null Element => default-construct (Emplace)
        void   (*RemoveAt)(void* Vector, int32 Index);
        void   (*Reset)(void* Vector);
        void   (*SetNum)(void* Vector, int32 NewNum);
        void   (*Reserve)(void* Vector, int32 Number);
        void   (*Swap)(void* Vector, int32 A, int32 B);
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
        uint32 ContainerSize;
        // Where the buffer pointer and the stored count sit, and what the count is divided by to give elements.
        uint32 DataOffset;
        uint32 CountOffset;
        uint32 CountDivisor;
    };

    // Ops for a specific vector-like CONTAINER type, not just its element type. TInlineVector<T, N> reflects as
    // a TVector<T>, but it embeds its buffer and carries a fixed allocator, so the table must construct and
    // grow the real type: operating on one through TVector<T>'s table would free the inline buffer as heap.
    template <typename TContainer>
    const FVectorOps* GetVectorOpsFor()
    {
        using T = typename TContainer::value_type;

        static const FVectorOps Ops =
        {
            [](const void* V) -> int32 { return static_cast<const TContainer*>(V)->Num(); },
            [](void* V) -> void* { return static_cast<TContainer*>(V)->GetData(); },
            [](void* V, const void* E) { TContainer* Vec = static_cast<TContainer*>(V); if (E) { Vec->Add(*static_cast<const T*>(E)); } else { Vec->Emplace(); } },
            [](void* V, int32 I) { static_cast<TContainer*>(V)->RemoveAt(I); },
            [](void* V) { static_cast<TContainer*>(V)->Reset(); },
            [](void* V, int32 N) { static_cast<TContainer*>(V)->SetNum(N); },
            [](void* V, int32 N) { static_cast<TContainer*>(V)->Reserve(N); },
            [](void* V, int32 A, int32 B) { static_cast<TContainer*>(V)->Swap(A, B); },
            static_cast<uint32>(sizeof(T)),
            [](void* V, const void*) { Memory::ConstructAt(static_cast<TContainer*>(V)); },
            [](void* V, const void*) { Memory::DestroyAt(static_cast<TContainer*>(V)); },
            nullptr,
            static_cast<uint32>(sizeof(TContainer)),
            TContainer::GetDataOffsetForInterop(),
            TContainer::GetCountOffsetForInterop(),
            1,
        };
        return &Ops;
    }

    template <typename T>
    const FVectorOps* GetVectorOps()
    {
        return GetVectorOpsFor<TVector<T>>();
    }

    // Type-erased op table for a THashMap<K,V>, the associative analogue of FVectorOps; one static table per
    // (K,V) via GetMapOps<K,V>(). Not index-addressable, so no GetData/GetAt/Swap/SetNum. Each captureless lambda
    // casts void* back to concrete K/V, so hashing/equality run on the real types.
    struct FMapOps
    {
        int32  (*Num)(const void* Map);
        // Insert-or-assign; ValuePtr null => value default-constructed. Returns the stored value slot.
        void*  (*Add)(void* Map, const void* KeyPtr, const void* ValuePtr);
        void*  (*Find)(void* Map, const void* KeyPtr);          // stored value for KeyPtr, or null
        bool   (*Remove)(void* Map, const void* KeyPtr);
        void   (*Reset)(void* Map);
        void   (*Reserve)(void* Map, int32 Number);
        void   (*ForEach)(const void* Map, void (*Visitor)(const void* KeyPtr, void* ValuePtr, void* UserData), void* UserData);
        // Placement construct/destruct a scratch key (deserialize path). Map is passed so a type-erased runtime
        // map can reach its key element description; the compile-time map ignores it.
        void   (*ConstructKey)(void* Map, void* Dst);
        void   (*DestructKey)(void* Map, void* Dst);
        // Key/value slot for the pair at iteration index (null out-params if out of range). Iteration order is
        // stable between mutations, so an editor can address entries by index. O(index) here, O(1) for the script map.
        void   (*At)(void* Map, int32 Index, const void** OutKey, void** OutValue);
        uint32 KeySize;
        uint32 ValueSize;
        // The map counterpart of FVectorOps::Construct/DestructContainer, Context and all: brings the MAP
        // itself up / tears it down in caller-owned raw memory, so FMapProperty can implement
        // ConstructValue/DestructValue without knowing whether it holds a THashMap<K,V> or a script map.
        void   (*ConstructContainer)(void* Map, const void* Context);
        void   (*DestructContainer)(void* Map, const void* Context);
        const void* ContainerContext;
        uint32 ContainerSize;
        // The next pair from an opaque Cursor that starts at zero, false at the end. O(1) amortized where At is O(index).
        bool   (*Next)(void* Map, int32* Cursor, const void** OutKey, void** OutValue);
    };

    // The map counterpart of GetVectorOpsFor: keyed on the container type so a TInlineHashMap<K, V, N>, which
    // reflects as a THashMap<K, V>, is still constructed and grown as the type it actually is.
    template <typename TContainer>
    const FMapOps* GetMapOpsFor()
    {
        using MapT = TContainer;
        using K = typename MapT::key_type;
        using V = typename MapT::mapped_type;
        static const FMapOps Ops =
        {
            [](const void* M) -> int32 { return static_cast<const MapT*>(M)->Num(); },
            [](void* M, const void* KP, const void* VP) -> void*
            {
                // Copied first, since either may live in this map and inserting can rehash it away.
                MapT* Map = static_cast<MapT*>(M);
                K Key = *static_cast<const K*>(KP);
                if (VP == nullptr)
                {
                    return &Map->FindOrAdd(Move(Key));
                }
                V Value = *static_cast<const V*>(VP);
                return &Map->Add(Move(Key), Move(Value));
            },
            [](void* M, const void* KP) -> void*
            {
                return static_cast<MapT*>(M)->Find(*static_cast<const K*>(KP));
            },
            [](void* M, const void* KP) -> bool
            {
                return static_cast<MapT*>(M)->Remove(*static_cast<const K*>(KP)) != 0;
            },
            [](void* M) { static_cast<MapT*>(M)->Reset(); },
            [](void* M, int32 N) { static_cast<MapT*>(M)->Reserve(N); },
            [](const void* M, void (*Visitor)(const void*, void*, void*), void* UserData)
            {
                // Only readers walk this, so the const_cast just fits the value to the void* signature.
                for (const auto& Pair : *static_cast<const MapT*>(M))
                {
                    Visitor(&Pair.Key, const_cast<V*>(&Pair.Value), UserData);
                }
            },
            [](void*, void* Dst) { Memory::ConstructAt(static_cast<K*>(Dst)); },
            [](void*, void* Dst) { Memory::DestroyAt(static_cast<K*>(Dst)); },
            [](void* M, int32 Index, const void** OutKey, void** OutValue)
            {
                MapT* Map = static_cast<MapT*>(M);
                int32 i = 0;
                for (auto& Pair : *Map)
                {
                    if (i++ == Index)
                    {
                        if (OutKey)   { *OutKey = &Pair.Key; }
                        if (OutValue) { *OutValue = &Pair.Value; }
                        return;
                    }
                }
                if (OutKey)   { *OutKey = nullptr; }
                if (OutValue) { *OutValue = nullptr; }
            },
            static_cast<uint32>(sizeof(K)),
            static_cast<uint32>(sizeof(V)),
            [](void* M, const void*) { Memory::ConstructAt(static_cast<MapT*>(M)); },
            [](void* M, const void*) { Memory::DestroyAt(static_cast<MapT*>(M)); },
            nullptr,
            static_cast<uint32>(sizeof(MapT)),
            [](void* M, int32* Cursor, const void** OutKey, void** OutValue) -> bool
            {
                auto* Pair = static_cast<MapT*>(M)->FindNextOccupied(*Cursor);
                if (Pair == nullptr)
                {
                    return false;
                }
                *OutKey = &Pair->Key;
                *OutValue = &Pair->Value;
                return true;
            },
        };
        return &Ops;
    }

    template <typename K, typename V>
    const FMapOps* GetMapOps()
    {
        return GetMapOpsFor<THashMap<K, V>>();
    }

    struct FOptionalOps
    {
        bool  (*IsSet)(const void* Optional);
        void* (*GetValue)(void* Optional);

        // A null value default-constructs the payload.
        void  (*SetValue)(void* Optional, const void* Value);
        void  (*Reset)(void* Optional);

        // Brings the optional itself up in raw memory, which frames and snapshots rely on to construct and destroy it.
        void  (*ConstructContainer)(void* Optional, const void* Context);
        void  (*DestructContainer)(void* Optional, const void* Context);
        const void* ContainerContext;
        uint32 ContainerSize;
    };

    template <typename TContainer>
    const FOptionalOps* GetOptionalOpsFor()
    {
        using T = typename TContainer::value_type;
        static const FOptionalOps Ops =
        {
            [](const void* O) { return static_cast<const TContainer*>(O)->IsSet(); },
            [](void* O) -> void*
            {
                TContainer* Optional = static_cast<TContainer*>(O);
                return Optional->IsSet() ? &Optional->GetValue() : nullptr;
            },
            [](void* O, const void* Value)
            {
                TContainer* Optional = static_cast<TContainer*>(O);
                if (Value != nullptr)
                {
                    *Optional = *static_cast<const T*>(Value);
                }
                else
                {
                    Optional->Emplace();
                }
            },
            [](void* O) { static_cast<TContainer*>(O)->Reset(); },
            [](void* O, const void*) { Memory::ConstructAt(static_cast<TContainer*>(O)); },
            [](void* O, const void*) { Memory::DestroyAt(static_cast<TContainer*>(O)); },
            nullptr,
            static_cast<uint32>(sizeof(TContainer)),
        };
        return &Ops;
    }
}
