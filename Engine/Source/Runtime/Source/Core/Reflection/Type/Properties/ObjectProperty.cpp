#include "RuntimePCH.h"
#include "ObjectProperty.h"

#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"

namespace Lumina::ObjectPropertyInternal
{
    // Code reading the field trusts its declared class, so a reference that resolved to another kind is dropped.
    CObject* Accept(const FObjectProperty& Property, CObject* Candidate)
    {
        CClass* Required = Property.GetPropertyClass();
        if (Candidate == nullptr || Required == nullptr || Candidate->IsA(Required))
        {
            return Candidate;
        }
        LOG_WARN("Object property '{}' expects a {} but was handed '{}', a {}; it was left empty.",
            Property.GetPropertyName(), Required->GetName(), Candidate->GetName(), Candidate->GetClass()->GetName());
        return nullptr;
    }
}

namespace Lumina
{
    void FObjectProperty::Serialize(FArchive& Ar, void* Value)
    {
        auto Ptr = static_cast<TStrongObjectPtr<CObject>*>(Value);

        CObject* Raw = Ptr->Get();
        Ar << Raw;
        *Ptr = Ar.IsReading() ? ObjectPropertyInternal::Accept(*this, Raw) : Raw;
    }

    void FObjectProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        auto Ptr = static_cast<TStrongObjectPtr<CObject>*>(Value);

        if (Ar.IsWriting())
        {
            CObject* Raw = Ptr->Get();

            // An indexed path uses a compact net index, with the GUID exported once via ObjectExport.
            if (Ar.ObjectToNetIndex)
            {
                WriteVarUInt(Ar, Raw ? Ar.ObjectToNetIndex(Raw) : 0u);
                return;
            }

            FGuid Guid = Raw ? Raw->GetGUID() : FGuid();
            Ar << Guid;
        }
        else
        {
            if (Ar.NetIndexToObject)
            {
                const uint32 Index = ReadVarUInt(Ar);
                *Ptr = ObjectPropertyInternal::Accept(*this, (Index != 0) ? Ar.NetIndexToObject(Index) : nullptr);
                return;
            }

            FGuid Guid;
            Ar << Guid;

            CObject* Resolved = nullptr;
            if (Guid.IsValid())
            {
                Resolved = FindObject<CObject>(Guid);
                if (Resolved == nullptr)
                {
                    Resolved = StaticLoadObject(Guid); // not resident yet -> try to load by GUID
                }
            }
            *Ptr = ObjectPropertyInternal::Accept(*this, Resolved);
        }
    }

    void FObjectProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        auto Ptr = static_cast<TStrongObjectPtr<CObject>*>(Value);

        CObject* Raw = Ptr->Get();
        Slot.Serialize(Raw);
        *Ptr = ObjectPropertyInternal::Accept(*this, Raw);
    }

    void FObjectProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        *static_cast<TStrongObjectPtr<CObject>*>(Dst) = *static_cast<const TStrongObjectPtr<CObject>*>(Src);
    }

    void FObjectProperty::DestructValue(void* Value) const
    {
        static_cast<TStrongObjectPtr<CObject>*>(Value)->~TStrongObjectPtr();
    }
}
