#include "RuntimePCH.h"
#include "StructProperty.h"

namespace Lumina
{
    void FStructProperty::Serialize(FArchive& Ar, void* Value)
    {
        Struct->SerializeTaggedProperties(Ar, Value);
    }

    void FStructProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        FArchiveRecord Record = Slot.EnterRecord();
        Struct->SerializeTaggedProperties(Record, Value, Defaults);
    }

    void FStructProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        // Uses the struct's NetSerializer (StructOps) if it has one, else recurses its fields.
        Struct->NetSerializeAll(Ar, Value);
    }

    bool FStructProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        if (FStructOps* Ops = Struct->GetStructOps(); Ops && Ops->HasEquality())
        {
            return Ops->Equals(ValueA, ValueB);
        }

        for (FProperty* Current : Struct->GetProperties())
        {
            if (!Current->Identical_InContainer(ValueA, ValueB))
            {
                return false;
            }
        }
        return true;
    }

    void FStructProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        if (FStructOps* Ops = Struct->GetStructOps(); Ops && Ops->HasCopy())
        {
            Ops->Copy(Dst, Src);
            return;
        }

        for (FProperty* Current : Struct->GetProperties())
        {
            Current->CopyCompleteValue_InContainer(Dst, Src);
        }
    }
}
