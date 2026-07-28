#include "pch.h"
#include "LuminaTypes.h"
#include "Core/Object/Field.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"

namespace Lumina
{
    // Base default: raw, tag-less value (works for numerics/enum/string/name/object-ref). Tight-packing
    // types (bool -> 1 bit, struct/array -> recurse) override NetSerialize on their own property class.
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

    void FProperty::Init()
    {
        eastl::visit([this](auto& Value)
        {
            Value->AddProperty(this);
        }, Owner);
   
    }
    
    const FName& FProperty::GetTypeName() const
    {
        return TypeName;
    }
    
    void FProperty::OnMetadataFinalized()
    {
        if (const FString* MaybeDisplayName = Metadata.TryGetMetadata("DisplayName"))
        {
            DisplayName = *MaybeDisplayName;
        }
        else
        {
            DisplayName = MakeDisplayNameFromName(TypeFlags, Name);
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
            Display[0] = eastl::CharToUpper(Display[0]);
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
