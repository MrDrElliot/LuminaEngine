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
        explicit FDelegateProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            SetElementSize(sizeof(FScriptDelegate));
        }

        // Each argument arrives as a SubField whose offset is into the broadcast arg pack.
        void AddProperty(FProperty* Property) override { Args.Add(Property); }

        /** Arguments in declaration order; offsets are into the arg pack a broadcast fills. */
        NODISCARD TSpan<FProperty* const> GetArgs() const { return TSpan<FProperty* const>(Args.GetData(), Args.Num()); }

        NODISCARD int32 GetNumArgs() const { return Args.Num(); }

        // Bindings are transient; every instance compares equal, a copy starts unbound, nothing to stringify.
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;
        RUNTIME_API FString ToString(const void* Data) const override;

        // Bindings are local, so a delegate carries nothing over the wire.
        void NetSerialize(FNetArchive& Ar, void* Value) override {}

        // Holds a listener list with a real destructor, so zeroed bytes are not a valid delegate.
        void ConstructValue(void* Value) const override { Memory::ConstructAt(static_cast<FScriptDelegate*>(Value)); }
        void DestructValue(void* Value) const override  { static_cast<FScriptDelegate*>(Value)->~FScriptDelegate(); }
        bool OwnsStorage() const override { return true; }

        void RelocateValue(void* Dest, void* Source) const override
        {
            Memory::ConstructAt(static_cast<FScriptDelegate*>(Dest), std::move(*static_cast<FScriptDelegate*>(Source)));
            static_cast<FScriptDelegate*>(Source)->~FScriptDelegate();
        }

    private:

        TVector<FProperty*> Args;
    };
}
