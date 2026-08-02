#include "RuntimePCH.h"
#include "StringProperty.h"
#include "Containers/Name.h"
#include "Core/Serialization/NetArchive.h"

namespace Lumina
{
    void FStringProperty::Serialize(FArchive& Ar, void* Value)
    {
        FString* StringValue = (FString*)Value;
        Ar << *StringValue;
    }

    void FStringProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        Slot.Serialize(*static_cast<FString*>(Value));
    }

    void FNameProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        Slot.Serialize(*static_cast<FName*>(Value));
    }

    void FNameProperty::Serialize(FArchive& Ar, void* Value)
    {
        FName* StringValue = (FName*)Value;
        Ar << *StringValue;
    }

    void FNameProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        FName* NameValue = static_cast<FName*>(Value);

        if (Ar.IsWriting())
        {
            // Indexed path (replication): a compact net index; the string is exported once via NameExport.
            if (Ar.NameToNetIndex)
            {
                WriteVarUInt(Ar, NameValue->IsNone() ? 0u : Ar.NameToNetIndex(*NameValue));
                return;
            }

            Ar << *NameValue;
        }
        else
        {
            if (Ar.NetIndexToName)
            {
                const uint32 Index = ReadVarUInt(Ar);
                if (Index != 0)
                {
                    Ar.NetIndexToName(Index, *NameValue);
                }
                else
                {
                    *NameValue = FName();
                }
                return;
            }

            Ar << *NameValue;
        }
    }

    bool FStringProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        return *static_cast<const FString*>(ValueA) == *static_cast<const FString*>(ValueB);
    }

    void FStringProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        *static_cast<FString*>(Dst) = *static_cast<const FString*>(Src);
    }
}
