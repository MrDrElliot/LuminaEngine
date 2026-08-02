#pragma once

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    /** Reflection wrapper for TOptional<T>. Wire format: bool engaged + optional payload. */
    class FOptionalProperty : public FProperty
    {
    public:
        FOptionalProperty(const FFieldOwner& InOwner, const FOptionalPropertyParams* Params)
            : FProperty(InOwner, Params)
            , HasValueFn(Params->HasValueFn)
            , GetValueFn(Params->GetValueFn)
            , SetValueFn(Params->SetValueFn)
            , ResetFn   (Params->ResetFn)
        {
        }

        DECLARE_FPROPERTY(EPropertyTypeFlags::Optional)

        /** Inner property (payload type) installed via ConstructProperties. */
        void AddProperty(FProperty* Property) override { Inner.reset(Property); }

        FProperty* GetInternalProperty() const { return Inner.get(); }

        bool  HasValue(const void* InContainer) const { return HasValueFn(InContainer); }
        void* GetValue(void* InContainer) const       { return GetValueFn(InContainer); }
        void  SetValue(void* InContainer, const void* InValue) const { SetValueFn(InContainer, InValue); }
        void  Reset(void* InContainer) const          { ResetFn(InContainer); }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        /** Compares engaged-state then payload via Inner; Copy mirrors engaged state. */
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

    private:

        OptionalHasValuePtr     HasValueFn;
        OptionalGetValuePtr     GetValueFn;
        OptionalSetValuePtr     SetValueFn;
        OptionalResetPtr        ResetFn;

        TUniquePtr<FProperty>   Inner;
    };
}
