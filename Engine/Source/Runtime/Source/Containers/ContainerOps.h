#pragma once

#include "Containers/HashTable.h"
#include "Containers/Optional.h"
#include "Memory/Construct.h"
#include "Containers/Vector.h"

namespace Lumina
{
    // Field order must match LuminaSharp.VectorOps (Core/ContainerOps.cs), which reads Add/RemoveAt/Reset at fixed offsets.
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
        // Construct and destruct the container in raw memory, so new fields must be appended after ElementSize.
        void   (*ConstructContainer)(void* Vector, const void* Context);
        void   (*DestructContainer)(void* Vector, const void* Context);
        const void* ContainerContext;
        uint32 ContainerSize;
        // Where the buffer pointer and the stored count sit, and what the count is divided by to give elements.
        uint32 DataOffset;
        uint32 CountOffset;
        uint32 CountDivisor;
    };

    // TInlineVector reflects as a TVector but embeds its buffer, so going through TVector's table would free it as heap.
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

    // Not index-addressable, so no GetData/GetAt/Swap/SetNum.
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
        // Map is passed so a type-erased runtime map can reach its key element description.
        void   (*ConstructKey)(void* Map, void* Dst);
        void   (*DestructKey)(void* Map, void* Dst);
        // Iteration order is stable between mutations, so an editor can address entries by index.
        void   (*At)(void* Map, int32 Index, const void** OutKey, void** OutValue);
        uint32 KeySize;
        uint32 ValueSize;
        // Construct and destruct the map in raw memory, so FMapProperty need not know if it holds a script map.
        void   (*ConstructContainer)(void* Map, const void* Context);
        void   (*DestructContainer)(void* Map, const void* Context);
        const void* ContainerContext;
        uint32 ContainerSize;
        // The next pair from an opaque Cursor that starts at zero, false at the end. O(1) amortized where At is O(index).
        bool   (*Next)(void* Map, int32* Cursor, const void** OutKey, void** OutValue);
    };

    // Keyed on the container type so a TInlineHashMap is still constructed as the type it actually is.
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
