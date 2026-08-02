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

        CStruct* GetPayloadStruct() const { return PayloadStruct; }

    private:

        CStruct* PayloadStruct = nullptr;
    };
}
