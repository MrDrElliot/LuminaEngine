#pragma once
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class CStruct;

    // Reflects a TScriptDelegate<T> member; stores the payload struct type, null for a no-payload delegate.
    class FDelegateProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::Delegate)

        FDelegateProperty(const FFieldOwner& InOwner, const FDelegatePropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            PayloadStruct = Params->PayloadStructFunc ? Params->PayloadStructFunc() : nullptr;
            SetElementSize(sizeof(FScriptDelegate));
        }

        // Bindings are transient; every instance compares equal, a copy starts unbound, nothing to stringify.
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;
        RUNTIME_API FString ToString(const void* Data) const override;

        // Holds a listener list with a real destructor, so zeroed bytes are not a valid delegate.
        void ConstructValue(void* Value) const override { new (Value) FScriptDelegate(); }
        void DestructValue(void* Value) const override  { static_cast<FScriptDelegate*>(Value)->~FScriptDelegate(); }
        bool OwnsStorage() const override { return true; }

        CStruct* GetPayloadStruct() const { return PayloadStruct; }

    private:

        CStruct* PayloadStruct = nullptr;
    };
}
