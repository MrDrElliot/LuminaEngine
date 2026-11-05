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
            uint32 WireCount = static_cast<uint32>(Num(Value));
            Ar.SerializeBits(&WireCount, 32);
            ForEach(Value, [&](const void* Key, void* Val)
            {
                KeyProperty->NetSerialize(Ar, const_cast<void*>(Key));
                ValueProperty->NetSerialize(Ar, Val);
            });
        }
        else
        {
            uint32 WireCount = 0;
            Ar.SerializeBits(&WireCount, 32);
            if (!Ar.CanHoldCount(WireCount))
            {
                LOG_ERROR("Map property '{}' was sent {} entries, more than the packet holds; failing the archive.", Name, WireCount);
                Ar.SetHasError(true);
                return;
            }
            const int32 Count = static_cast<int32>(WireCount);
            Reset(Value);
            Reserve(Value, Count);

            void* KeyScratch = Memory::Malloc(GetKeySize(), 16);
            for (int32 i = 0; i < Count && !Ar.HasError(); ++i)
            {
                Ops->ConstructKey(Value, KeyScratch);
                KeyProperty->NetSerialize(Ar, KeyScratch);
                void* Slot = Add(Value, KeyScratch, nullptr);
                ValueProperty->NetSerialize(Ar, Slot);
                Ops->DestructKey(Value, KeyScratch);
            }
            Memory::Free(KeyScratch);
        }
    }

    void FMapProperty::Serialize(FArchive& Ar, void* Value)
    {
        uint64 SerializedCount = Num(Value);
        Ar << SerializedCount;

        // A map never bulk-copies, so a size mismatch is detectable but not fatal, and pairs round-trip.
        size_t SerializedKeySize   = KeyProperty->GetElementSize();
        size_t SerializedValueSize = ValueProperty->GetElementSize();
        Ar << SerializedKeySize;
        Ar << SerializedValueSize;

        if (SerializedCount > static_cast<uint64>(TNumericLimits<int32>::Max()) || !Ar.CanHoldCount(SerializedCount))
        {
            LOG_ERROR("Map property '{}' tried to serialize {} entries. Aborted", Name, SerializedCount);
            Ar.SetHasError(true);
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
            const int32 Count = static_cast<int32>(SerializedCount);
            Reset(Value);
            Reserve(Value, Count);

            void* KeyScratch = Memory::Malloc(GetKeySize(), 16);
            for (int32 i = 0; i < Count; ++i)
            {
                Ops->ConstructKey(Value, KeyScratch);
                KeyProperty->Serialize(Ar, KeyScratch);
                void* Slot = Add(Value, KeyScratch, nullptr);
                ValueProperty->Serialize(Ar, Slot);
                Ops->DestructKey(Value, KeyScratch);
            }
            Memory::Free(KeyScratch);
        }
    }

    void FMapProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* /*Defaults*/)
    {
        // Uses only the array slot API, so it round-trips uniformly with no per-pair record shape.
        int32 NumSlots = Num(Value) * 2;
        FArchiveArray Array = Slot.EnterArray(NumSlots);

        if (Slot.GetArchiver().IsReading())
        {
            const int32 PairCount = NumSlots / 2;
            Reset(Value);
            Reserve(Value, PairCount);

            void* KeyScratch = Memory::Malloc(GetKeySize(), 16);
            for (int32 i = 0; i < PairCount; ++i)
            {
                Ops->ConstructKey(Value, KeyScratch);
                KeyProperty->SerializeItem(Array.EnterElement(), KeyScratch);
                void* ValueSlot = Add(Value, KeyScratch, nullptr);
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
        if (Num(ValueA) != Num(ValueB))
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
        if (Dst == Src)
        {
            return;
        }
        Reset(Dst);
        Reserve(Dst, Num(Src));
        ForEach(Src, [&](const void* Key, void* Val)
        {
            // Add copies the key and default-constructs the value; then deep-copy the value into that slot.
            void* Slot = Add(Dst, Key, nullptr);
            ValueProperty->CopyCompleteValue(Slot, Val);
        });
    }
}
