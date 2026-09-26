#pragma once
#include "Core/Delegates/ScriptDelegate.h"
#include "Memory/Construct.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class CStruct;

    // Reflects a TScriptDelegate<Args...> member; the arguments are reflected properties over the arg pack.
    class FDelegateProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::Delegate)

        explicit FDelegateProperty(const FDelegatePropertyParams* Params)
            : FProperty(Params)
        {
            SetElementSize(sizeof(FScriptDelegate));
        }

        // Each argument arrives as a SubField whose offset is into the broadcast arg pack.
        void AddProperty(FProperty* Property) override { Args.push_back(Property); }

        /** Arguments in declaration order; offsets are into the arg pack a broadcast fills. */
        NODISCARD TSpan<FProperty* const> GetArgs() const { return TSpan<FProperty* const>(Args.data(), Args.size()); }

        NODISCARD size_t GetNumArgs() const { return Args.size(); }

        // Bindings are transient; every instance compares equal, a copy starts unbound, nothing to stringify.
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;
        RUNTIME_API FString ToString(const void* Data) const override;

        // Holds a listener list with a real destructor, so zeroed bytes are not a valid delegate.
        void ConstructValue(void* Value) const override { Memory::ConstructAt(static_cast<FScriptDelegate*>(Value)); }
        void DestructValue(void* Value) const override  { static_cast<FScriptDelegate*>(Value)->~FScriptDelegate(); }
        bool OwnsStorage() const override { return true; }

    private:

        TVector<FProperty*> Args;
    };
}
