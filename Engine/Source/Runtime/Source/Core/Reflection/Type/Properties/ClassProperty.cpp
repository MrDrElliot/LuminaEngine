#include "RuntimePCH.h"
#include "ClassProperty.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    CClass* FClassProperty::ResolveSerializedClass(const FName& ClassName) const
    {
        if (ClassName.IsNone())
        {
            return nullptr;
        }
        CClass* Resolved = FindObject<CClass>(ClassName);
        // A name resolving to a non-derived class yields null rather than a wrong-typed CClass.
        if (Resolved != nullptr && MetaClass != nullptr && !Resolved->IsChildOf(MetaClass))
        {
            return nullptr;
        }
        return Resolved;
    }

    void FClassProperty::Serialize(FArchive& Ar, void* Value)
    {
        auto Ptr = static_cast<CClass**>(Value);

        if (Ar.IsReading())
        {
            FName ClassName;
            Ar << ClassName;
            *Ptr = ResolveSerializedClass(ClassName);
        }
        else
        {
            FName ClassName = *Ptr ? (*Ptr)->GetName() : NAME_None;
            Ar << ClassName;
        }
    }

    void FClassProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        auto Ptr = static_cast<CClass**>(Value);

        if (Slot.GetArchiver().IsReading())
        {
            FName ClassName;
            Slot.Serialize(ClassName);
            *Ptr = ResolveSerializedClass(ClassName);
        }
        else
        {
            FName ClassName = *Ptr ? (*Ptr)->GetName() : NAME_None;
            Slot.Serialize(ClassName);
        }
    }
}
