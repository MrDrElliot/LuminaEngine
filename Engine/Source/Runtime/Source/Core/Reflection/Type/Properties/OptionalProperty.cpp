#include "RuntimePCH.h"
#include "OptionalProperty.h"
#include "Core/Serialization/NetArchive.h"

namespace Lumina
{
    void FOptionalProperty::Serialize(FArchive& Ar, void* Value)
    {
        // The wire carries an engaged bool followed by the payload only when engaged.
        if (Ar.IsWriting())
        {
            bool bEngaged = IsSet(Value);
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
            bool bEngaged = IsSet(Value);
            Record << StructuredArchive::TNamedValue<bool>("Engaged", bEngaged);

            if (bEngaged)
            {
                Inner->SerializeItem(Record.EnterField("Value"), GetValue(Value));
            }
        }
    }

    bool FOptionalProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        const bool bA = IsSet(ValueA);
        const bool bB = IsSet(ValueB);
        if (bA != bB)
        {
            return false;
        }

        if (!bA)
        {
            return true;
        }

        const void* PayloadA = Ops->GetValue(const_cast<void*>(ValueA));
        const void* PayloadB = Ops->GetValue(const_cast<void*>(ValueB));
        return Inner->Identical(PayloadA, PayloadB);
    }

    void FOptionalProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        bool bEngaged = Ar.IsWriting() && IsSet(Value);
        Ar.SerializeBit(bEngaged);
        if (!bEngaged)
        {
            if (Ar.IsReading())
            {
                Reset(Value);
            }
            return;
        }
        if (Ar.IsReading() && !IsSet(Value))
        {
            SetValue(Value, nullptr);
        }
        if (Inner != nullptr)
        {
            Inner->NetSerialize(Ar, GetValue(Value));
        }
    }

    void FOptionalProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        if (Dst == Src)
        {
            return;
        }
        const bool bSrcEngaged = IsSet(Src);
        if (!bSrcEngaged)
        {
            Reset(Dst);
            return;
        }

        SetValue(Dst, nullptr);
        void* DstPayload = Ops->GetValue(Dst);
        const void* SrcPayload = Ops->GetValue(const_cast<void*>(Src));
        Inner->CopyCompleteValue(DstPayload, SrcPayload);
    }
}
