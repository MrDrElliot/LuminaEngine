#pragma once
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class FStructProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::Struct)

        FStructProperty(const FFieldOwner& InOwner, const FPropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            auto* StructParams = (const FStructPropertyParams*)Params;
            CStruct* InternalStruct = StructParams->StructFunc();
            ASSERT(InternalStruct);
            SetStruct(InternalStruct);
            SetElementSize(Struct->GetAlignedSize());
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Nested struct: defer to the struct (its net serializer if it has one, else recurse fields).
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        void SetStruct(CStruct* InStruct) { Struct = InStruct; }
        CStruct* GetStruct() const { return Struct; }

        // Uses FStructOps::Equals/Copy when the struct opted in via operator==
        // / CopyFrom; otherwise walks the linked property list and recurses.
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        
        CStruct* Struct = nullptr;
    
    };
}
