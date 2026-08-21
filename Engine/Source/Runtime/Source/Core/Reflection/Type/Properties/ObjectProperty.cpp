#include "RuntimePCH.h"
#include "ObjectProperty.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/NetArchive.h"


namespace Lumina
{

    void FObjectProperty::Serialize(FArchive& Ar, void* Value)
    {
        auto Ptr = static_cast<TObjectPtr<CObject>*>(Value);

        CObject* Raw = Ptr->Get();
        Ar << Raw;
        *Ptr = Raw;
    }

    void FObjectProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        auto Ptr = static_cast<TObjectPtr<CObject>*>(Value);

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
                *Ptr = (Index != 0) ? Ar.NetIndexToObject(Index) : nullptr;
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
            *Ptr = Resolved;
        }
    }

    void FObjectProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        auto Ptr = static_cast<TObjectPtr<CObject>*>(Value);

        CObject* Raw = Ptr->Get();
        Slot.Serialize(Raw);
        *Ptr = Raw;
    }

    void FObjectProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        *static_cast<TObjectPtr<CObject>*>(Dst) = *static_cast<const TObjectPtr<CObject>*>(Src);
    }

    void FObjectProperty::DestructValue(void* Value) const
    {
        static_cast<TObjectPtr<CObject>*>(Value)->~TObjectPtr();
    }
}
