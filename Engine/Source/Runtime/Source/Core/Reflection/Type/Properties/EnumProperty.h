#pragma once
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FEnumProperty : public FProperty
    {
    public:

        FEnumProperty(const FFieldOwner& InOwner, const FPropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            auto* EnumParams = static_cast<const FEnumPropertyParams*>(Params);
            CEnum* InternalEnum = EnumParams->EnumFunc();
            ASSERT(InternalEnum);
            SetEnum(InternalEnum);
        }
        
        void AddProperty(FProperty* Property) override { InnerProperty.reset(static_cast<FNumericProperty*>(Property)); }
        RUNTIME_API FNumericProperty* GetInnerProperty() const { return InnerProperty.get(); }

        void SetEnum(CEnum* InEnum);

        FORCEINLINE CEnum* GetEnum() const { return Enum; }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // FEnumProperty stores its value through the inner numeric property and is NOT a TProperty<>, so
        // its own ElementSize is never set -- the base memcpy/memcmp would be zero-length no-ops (which
        // silently broke reset-to-default / Identical for enum members of a struct). Delegate to the
        // inner property, which carries the correct underlying-type size.
        void CopyCompleteValue(void* Dst, const void* Src) const override
        {
            if (InnerProperty) { InnerProperty->CopyCompleteValue(Dst, Src); }
        }

        bool Identical(const void* ValueA, const void* ValueB) const override
        {
            return InnerProperty ? InnerProperty->Identical(ValueA, ValueB) : true;
        }


    private:

        TUniquePtr<FNumericProperty> InnerProperty;
        CEnum* Enum = nullptr;
    };
}
