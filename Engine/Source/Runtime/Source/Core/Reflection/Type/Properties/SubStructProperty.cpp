#include "RuntimePCH.h"
#include "SubStructProperty.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    void FSubStructProperty::Serialize(FArchive& Ar, void* Value)
    {
        auto Ptr = static_cast<CStruct**>(Value);

        if (Ar.IsReading())
        {
            FName StructName;
            Ar << StructName;
            *Ptr = StructName.IsNone() ? nullptr : FindObject<CStruct>(StructName);
        }
        else
        {
            FName StructName = *Ptr ? (*Ptr)->GetName() : NAME_None;
            Ar << StructName;
        }
    }

    void FSubStructProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        auto Ptr = static_cast<CStruct**>(Value);

        if (Slot.GetArchiver().IsReading())
        {
            FName StructName;
            Slot.Serialize(StructName);
            *Ptr = StructName.IsNone() ? nullptr : FindObject<CStruct>(StructName);
        }
        else
        {
            FName StructName = *Ptr ? (*Ptr)->GetName() : NAME_None;
            Slot.Serialize(StructName);
        }
    }
}
