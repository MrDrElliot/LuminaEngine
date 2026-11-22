#pragma once
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FEnumProperty : public FProperty
    {
    public:

        explicit FEnumProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            CEnum* InternalEnum = InDesc.Type->GetEnum();
            ASSERT(InternalEnum);
            SetEnum(InternalEnum);
        }
        
        void AddProperty(FProperty* Property) override { InnerProperty = static_cast<FNumericProperty*>(Property); }
        RUNTIME_API FNumericProperty* GetInnerProperty() const { return InnerProperty; }

        void SetEnum(CEnum* InEnum);

        FORCEINLINE CEnum* GetEnum() const { return Enum; }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Not a TProperty, so its ElementSize is never set and the base memcpy and memcmp would be zero-length no-ops. Delegates to the inner property.
        void CopyCompleteValue(void* Dst, const void* Src) const override
        {
            if (InnerProperty) { InnerProperty->CopyCompleteValue(Dst, Src); }
        }

        bool Identical(const void* ValueA, const void* ValueB) const override
        {
            return InnerProperty ? InnerProperty->Identical(ValueA, ValueB) : true;
        }


    private:

        FNumericProperty* InnerProperty = nullptr;
        CEnum* Enum = nullptr;
    };
}
