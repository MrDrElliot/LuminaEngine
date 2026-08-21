#include "RuntimePCH.h"
#include "MapProperty.h"
#include "Core/Serialization/NetArchive.h"
#include "Memory/Memory.h"
#include "Log/Log.h"

namespace Lumina
{
    // One reusable scratch buffer per call, since a hash map cannot create a slot before the key exists.

    void FMapProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        if (Ar.IsWriting())
        {
            uint32 Num = static_cast<uint32>(GetNum(Value));
            Ar.SerializeBits(&Num, 32);
            ForEach(Value, [&](const void* Key, void* Val)
            {
                KeyProperty->NetSerialize(Ar, const_cast<void*>(Key));
                ValueProperty->NetSerialize(Ar, Val);
            });
        }
        else
        {
            uint32 Num = 0;
            Ar.SerializeBits(&Num, 32);
            Clear(Value);
            Reserve(Value, Num);

            void* KeyScratch = Memory::Malloc(GetKeySize(), 16);
            for (uint32 i = 0; i < Num && !Ar.HasError(); ++i)
            {
                Ops->ConstructKey(Value, KeyScratch);
                KeyProperty->NetSerialize(Ar, KeyScratch);
                void* Slot = Insert(Value, KeyScratch, nullptr);
                ValueProperty->NetSerialize(Ar, Slot);
                Ops->DestructKey(Value, KeyScratch);
            }
            Memory::Free(KeyScratch);
        }
    }

    void FMapProperty::Serialize(FArchive& Ar, void* Value)
    {
        SIZE_T Count = GetNum(Value);
        Ar << Count;

        // A map never bulk-copies, so a size mismatch is detectable but not fatal, and pairs round-trip.
        size_t SerializedKeySize   = KeyProperty->GetElementSize();
        size_t SerializedValueSize = ValueProperty->GetElementSize();
        Ar << SerializedKeySize;
        Ar << SerializedValueSize;

        if (Count > 0xFFFFFFFFull)
        {
            LOG_ERROR("Map property '{}' tried to serialize {} entries. Aborted", Name, Count);
            return;
        }

        if (Ar.IsWriting())
        {
            ForEach(Value, [&](const void* Key, void* Val)
            {
                KeyProperty->Serialize(Ar, const_cast<void*>(Key));
                ValueProperty->Serialize(Ar, Val);
            });
        }
        else
        {
            Clear(Value);
            Reserve(Value, Count);

            void* KeyScratch = Memory::Malloc(GetKeySize(), 16);
            for (SIZE_T i = 0; i < Count; ++i)
            {
                Ops->ConstructKey(Value, KeyScratch);
                KeyProperty->Serialize(Ar, KeyScratch);
                void* Slot = Insert(Value, KeyScratch, nullptr);
                ValueProperty->Serialize(Ar, Slot);
                Ops->DestructKey(Value, KeyScratch);
            }
            Memory::Free(KeyScratch);
        }
    }

    void FMapProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* /*Defaults*/)
    {
        // Uses only the array slot API, so it round-trips uniformly with no per-pair record shape.
        int32 NumSlots = static_cast<int32>(GetNum(Value)) * 2;
        FArchiveArray Array = Slot.EnterArray(NumSlots);

        if (Slot.GetArchiver().IsReading())
        {
            const int32 PairCount = NumSlots / 2;
            Clear(Value);
            Reserve(Value, static_cast<size_t>(PairCount));

            void* KeyScratch = Memory::Malloc(GetKeySize(), 16);
            for (int32 i = 0; i < PairCount; ++i)
            {
                Ops->ConstructKey(Value, KeyScratch);
                KeyProperty->SerializeItem(Array.EnterElement(), KeyScratch);
                void* ValueSlot = Insert(Value, KeyScratch, nullptr);
                ValueProperty->SerializeItem(Array.EnterElement(), ValueSlot);
                Ops->DestructKey(Value, KeyScratch);
            }
            Memory::Free(KeyScratch);
        }
        else
        {
            ForEach(Value, [&](const void* Key, void* Val)
            {
                KeyProperty->SerializeItem(Array.EnterElement(), const_cast<void*>(Key));
                ValueProperty->SerializeItem(Array.EnterElement(), Val);
            });
        }
    }

    bool FMapProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        if (GetNum(ValueA) != GetNum(ValueB))
        {
            return false;
        }

        // Counts match, so a per-key value compare from A into B is order-independent and sufficient.
        bool bEqual = true;
        ForEach(ValueA, [&](const void* Key, void* ValA)
        {
            if (!bEqual)
            {
                return;
            }
            void* ValB = Ops->Find(const_cast<void*>(ValueB), Key);
            if (ValB == nullptr || !ValueProperty->Identical(ValA, ValB))
            {
                bEqual = false;
            }
        });
        return bEqual;
    }

    void FMapProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        Clear(Dst);
        Reserve(Dst, GetNum(Src));
        ForEach(Src, [&](const void* Key, void* Val)
        {
            // Insert copies the key and default-constructs the value; then deep-copy the value into that slot.
            void* Slot = Insert(Dst, Key, nullptr);
            ValueProperty->CopyCompleteValue(Slot, Val);
        });
    }
}
