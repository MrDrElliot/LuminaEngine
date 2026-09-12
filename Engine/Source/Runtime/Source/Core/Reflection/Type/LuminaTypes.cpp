#include "RuntimePCH.h"
#include "LuminaTypes.h"
#include "Core/Object/Field.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"

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
    
    FCStringView FProperty::GetMetadata(FStringView Key) const
    {
        // A property carries a handful of pairs, so a scan beats hashing and never touches the name pool.
        // Case-insensitive because the keys used to be FNames, whose comparison is case-folded.
        for (uint16 Index = 0; Index < NumMetadata; ++Index)
        {
            if (EqualsIgnoreCase(Key, FStringView(MetadataEntries[Index].NameUTF8)))
            {
                return FCStringView(MetadataEntries[Index].ValueUTF8);
            }
        }
        return FCStringView();
    }

    bool FProperty::HasMetadata(FStringView Key) const
    {
        for (uint16 Index = 0; Index < NumMetadata; ++Index)
        {
            if (EqualsIgnoreCase(Key, FStringView(MetadataEntries[Index].NameUTF8)))
            {
                return true;
            }
        }
        return false;
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
            if (i > 0 && std::isupper(Raw[i]) && !std::isspace(Raw[i - 1]) && !std::isupper(Raw[i - 1]))
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
    
    void FProperty::CallSetter(void* Container, const void* InValue) const
    {
        if (!HasSetter())
        {
            LOG_CRITICAL("Calling a setter but the property has no setter defined.");
        }
    }

    void FProperty::CallGetter(const void* Container, void* OutValue) const
    {
        if (!HasGetter())
        {
            LOG_CRITICAL("Calling a getter but the property has no getter defined.");
        }
    }

    void* FProperty::GetValuePtrInternal(void* ContainerPtr, int64 ArrayIndex) const
    {
        void* PropertyPtr = (uint8*)ContainerPtr + Offset;
        return (uint8*)PropertyPtr + ArrayIndex * ElementSize;
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
