#include "RuntimePCH.h"
#include "LuminaTypes.h"
#include "Core/Object/Field.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"
#include <charconv>
#include "Properties/ArrayProperty.h"
#include "Properties/EnumProperty.h"
#include "Properties/MapProperty.h"
#include "Properties/StructProperty.h"

namespace Lumina
{
    // Well inside a cache line. Properties are among the most numerous objects in the engine, so the block
    // stays packed; this catches a member added without checking where it lands as much as one that grows it.
    static_assert(sizeof(FProperty) == 56, "FProperty changed size; re-check its member order.");

    // Tight-packing types such as bool and struct override NetSerialize on their own property class.
    void FProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        Serialize(Ar, Value);
    }

    void FEntityProperty::Serialize(FArchive& Ar, void* Value)
    {
        Ar.SerializeEntityId(*static_cast<uint32*>(Value));
    }

    void FEntityProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        uint32& Raw = *static_cast<uint32*>(Value);
        if (Ar.IsWriting() && Ar.EntityToNetGUID)
        {
            WriteVarUInt(Ar, Ar.EntityToNetGUID(Raw));
            return;
        }
        if (Ar.IsReading() && Ar.NetGUIDToEntity)
        {
            Raw = Ar.NetGUIDToEntity(ReadVarUInt(Ar));
            return;
        }
        Super::NetSerialize(Ar, Value);
    }

    void FBoolProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        bool bValue = *static_cast<bool*>(Value);
        Ar.SerializeBit(bValue);
        *static_cast<bool*>(Value) = bValue; // no-op when writing
    }

    namespace
    {
        // TypeFlags already names the type, so the FName is looked up rather than stored on every property.
        struct FPropertyTypeNames
        {
            FPropertyTypeNames()
            {
                for (size_t Index = 0; Index < std::size(Names); ++Index)
                {
                    Names[Index] = FName(PropertyTypeFlagNames[Index]);
                }
            }

            FName Names[(size_t)EPropertyTypeFlags::Count];
        };
    }

    const FName& FProperty::GetTypeName() const
    {
        static const FPropertyTypeNames Table;

        DEBUG_ASSERT(TypeFlags < EPropertyTypeFlags::Count);
        return Table.Names[(size_t)TypeFlags];
    }
    
    const FMetadataEntry* FProperty::FindMetadata(FStringView Key) const
    {
        // A property carries a handful of entries, so a scan beats hashing. Case-insensitive like the FNames the keys once were.
        for (uint16 Index = 0; Index < Desc->NumMetadata; ++Index)
        {
            if (EqualsIgnoreCase(Key, FStringView(Desc->Metadata[Index].Key)))
            {
                return &Desc->Metadata[Index];
            }
        }
        return nullptr;
    }

    FCStringView FProperty::GetMetadata(FStringView Key) const
    {
        const FMetadataEntry* Entry = FindMetadata(Key);
        return Entry != nullptr ? FCStringView(Entry->Value) : FCStringView();
    }

    bool FProperty::HasMetadata(FStringView Key) const
    {
        return FindMetadata(Key) != nullptr;
    }

    TOptional<double> FProperty::GetMetadataNumber(FStringView Key) const
    {
        FStringView Text = GetMetadata(Key);
        if (!Text.empty() && Text.front() == '+')
        {
            Text.remove_prefix(1);
        }
        if (!Text.empty() && (Text.back() == 'f' || Text.back() == 'F'))
        {
            Text.remove_suffix(1);
        }

        double Value = 0.0;
        const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
        if (Text.empty() || Error != std::errc() || End != Text.data() + Text.size())
        {
            return {};
        }
        return Value;
    }

    void FProperty::OnMetadataFinalized(FPropertyArena& Arena)
    {
        // An authored name already lives in the metadata table, so only a derived one needs arena storage.
        if (const FCStringView MaybeDisplayName = GetMetadata("DisplayName"); !MaybeDisplayName.empty())
        {
            DisplayName = MaybeDisplayName.c_str();
        }
        else
        {
            DisplayName = Arena.CopyString(MakeDisplayNameFromName(TypeFlags, Name));
        }
    }

    FString FProperty::MakeDisplayNameFromName(EPropertyTypeFlags TypeFlags, const FName& InName)
    {
        FFixedString Raw = InName.c_str();
        FStringView View(Raw.begin(), Raw.length());
        
        if (TypeFlags == EPropertyTypeFlags::Bool)
        {
            if (View.starts_with('b') && std::isupper(Raw[1]))
            {
                Raw.erase(0, 1);
            }
        }

        FString Display;
        for (size_t i = 0; i < Raw.size(); ++i)
        {
            const bool bAcronymEnds = i > 0 && i + 1 < Raw.size() && std::isupper(Raw[i - 1]) && std::islower(Raw[i + 1]);
            if (i > 0 && std::isupper(Raw[i]) && !std::isspace(Raw[i - 1]) && (!std::isupper(Raw[i - 1]) || bAcronymEnds))
            {
                Display += ' ';
            }
            Display += Raw[i];
        }

        if (!Display.empty())
        {
            Display[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(Display[0])));
        }

        return Display;
    }
    
    void* FProperty::GetValuePtrInternal(void* ContainerPtr, int64 ArrayIndex) const
    {
        void* PropertyPtr = (uint8*)ContainerPtr + Offset;
        return (uint8*)PropertyPtr + ArrayIndex * ElementSize;
    }

    bool FProperty::HasSameValueType(const FProperty* Other) const
    {
        if (Other == nullptr || TypeFlags != Other->TypeFlags || GetElementSize() != Other->GetElementSize())
        {
            return false;
        }
        switch (TypeFlags)
        {
            case EPropertyTypeFlags::Struct:
                return static_cast<const FStructProperty*>(this)->GetStruct() == static_cast<const FStructProperty*>(Other)->GetStruct();
            case EPropertyTypeFlags::Enum:
                return static_cast<const FEnumProperty*>(this)->GetEnum() == static_cast<const FEnumProperty*>(Other)->GetEnum();
            case EPropertyTypeFlags::Vector:
            {
                const FProperty* Inner = static_cast<const FArrayProperty*>(this)->GetInternalProperty();
                return Inner != nullptr && Inner->HasSameValueType(static_cast<const FArrayProperty*>(Other)->GetInternalProperty());
            }
            case EPropertyTypeFlags::Map:
            {
                const FMapProperty* Map = static_cast<const FMapProperty*>(this);
                const FMapProperty* OtherMap = static_cast<const FMapProperty*>(Other);
                return Map->GetKeyProperty() != nullptr && Map->GetValueProperty() != nullptr
                    && Map->GetKeyProperty()->HasSameValueType(OtherMap->GetKeyProperty())
                    && Map->GetValueProperty()->HasSameValueType(OtherMap->GetValueProperty());
            }
            default:
                return true;
        }
    }

    bool FProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        return memcmp(ValueA, ValueB, ElementSize) == 0;
    }

    void FProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        memcpy(Dst, Src, ElementSize);
    }

    bool FProperty::Identical_InContainer(const void* ContainerA, const void* ContainerB, int64 ArrayIndex) const
    {
        const void* A = GetValuePtrInternal(const_cast<void*>(ContainerA), ArrayIndex);
        const void* B = GetValuePtrInternal(const_cast<void*>(ContainerB), ArrayIndex);
        return Identical(A, B);
    }

    void FProperty::CopyCompleteValue_InContainer(void* DstContainer, const void* SrcContainer, int64 ArrayIndex) const
    {
        void* D = GetValuePtrInternal(DstContainer, ArrayIndex);
        const void* S = GetValuePtrInternal(const_cast<void*>(SrcContainer), ArrayIndex);
        CopyCompleteValue(D, S);
    }
}
