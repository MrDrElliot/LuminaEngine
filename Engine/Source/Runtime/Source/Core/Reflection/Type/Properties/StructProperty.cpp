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

        FProperty* Current = Struct->LinkedProperty;
        while (Current != nullptr)
        {
            if (!Current->Identical_InContainer(ValueA, ValueB))
            {
                return false;
            }
            Current = static_cast<FProperty*>(Current->Next);
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

        FProperty* Current = Struct->LinkedProperty;
        while (Current != nullptr)
        {
            Current->CopyCompleteValue_InContainer(Dst, Src);
            Current = static_cast<FProperty*>(Current->Next);
        }
    }
}
