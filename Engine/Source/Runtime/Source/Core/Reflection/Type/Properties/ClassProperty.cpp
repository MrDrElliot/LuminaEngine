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
        // Enforce the TSubclassOf<T> contract that the raw *Ptr write below bypasses: a name that resolves to a
        // class which isn't MetaClass-derived (hand-edited config, a reparented or colliding class) yields null
        // instead of a wrong-typed CClass that callers would Cast<T> to null and then dereference.
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
