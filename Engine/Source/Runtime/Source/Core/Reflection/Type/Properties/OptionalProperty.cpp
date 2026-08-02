#include "RuntimePCH.h"
#include "OptionalProperty.h"

namespace Lumina
{
    void FOptionalProperty::Serialize(FArchive& Ar, void* Value)
    {
        // Wire: [bool engaged][payload?]
        if (Ar.IsWriting())
        {
            bool bEngaged = HasValue(Value);
            Ar << bEngaged;

            if (bEngaged)
            {
                Inner->Serialize(Ar, GetValue(Value));
            }
        }
        else
        {
            bool bEngaged = false;
            Ar << bEngaged;

            if (bEngaged)
            {
                // SetValue(null) engages via default-construct; we then deserialize into the slot.
                SetValue(Value, nullptr);
                Inner->Serialize(Ar, GetValue(Value));
            }
            else
            {
                Reset(Value);
            }
        }
    }

    void FOptionalProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        FArchiveRecord Record = Slot.EnterRecord();

        if (Slot.GetArchiver().IsReading())
        {
            bool bEngaged = false;
            Record << StructuredArchive::TNamedValue<bool>("Engaged", bEngaged);

            if (bEngaged)
            {
                SetValue(Value, nullptr);
                Inner->SerializeItem(Record.EnterField("Value"), GetValue(Value));
            }
            else
            {
                Reset(Value);
            }
        }
        else
        {
            bool bEngaged = HasValue(Value);
            Record << StructuredArchive::TNamedValue<bool>("Engaged", bEngaged);

            if (bEngaged)
            {
                Inner->SerializeItem(Record.EnterField("Value"), GetValue(Value));
            }
        }
    }

    bool FOptionalProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        const bool bA = HasValue(ValueA);
        const bool bB = HasValue(ValueB);
        if (bA != bB)
        {
            return false;
        }

        if (!bA)
        {
            return true;
        }

        const void* PayloadA = GetValueFn(const_cast<void*>(ValueA));
        const void* PayloadB = GetValueFn(const_cast<void*>(ValueB));
        return Inner->Identical(PayloadA, PayloadB);
    }

    void FOptionalProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        const bool bSrcEngaged = HasValue(Src);
        if (!bSrcEngaged)
        {
            Reset(Dst);
            return;
        }

        SetValue(Dst, nullptr);
        void* DstPayload = GetValueFn(Dst);
        const void* SrcPayload = GetValueFn(const_cast<void*>(Src));
        Inner->CopyCompleteValue(DstPayload, SrcPayload);
    }
}
