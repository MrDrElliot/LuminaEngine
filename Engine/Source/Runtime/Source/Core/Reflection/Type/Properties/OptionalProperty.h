#pragma once

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Containers/ContainerOps.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    /** Reflection wrapper for TOptional<T>. Wire format: bool engaged + optional payload. */
    class LUMINA_VISIBLE_TYPE FOptionalProperty : public FProperty
    {
    public:
        explicit FOptionalProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
            , Ops(InDesc.Type->GetOptionalOps())
        {
            SetElementSize(Ops != nullptr ? Ops->ContainerSize : 0);
        }

        void AddProperty(FProperty* Property) override { Inner = Property; }

        FProperty* GetInternalProperty() const { return Inner; }

        bool  IsSet(const void* InContainer) const    { return Ops->IsSet(InContainer); }
        void* GetValue(void* InContainer) const       { return Ops->GetValue(InContainer); }
        void  SetValue(void* InContainer, const void* InValue) const { Ops->SetValue(InContainer, InValue); }
        void  Reset(void* InContainer) const          { Ops->Reset(InContainer); }

        // Routed through the ops rather than a type test, so a TOptional<T> and a script optional are one path.
        void ConstructValue(void* Value) const override { if (Ops->ConstructContainer) { Ops->ConstructContainer(Value, Ops->ContainerContext); } }
        void DestructValue(void* Value) const override  { if (Ops->DestructContainer)  { Ops->DestructContainer(Value, Ops->ContainerContext); } }
        bool OwnsStorage() const override { return Ops->ConstructContainer != nullptr; }

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        /** Compares engaged-state then payload via Inner; Copy mirrors engaged state. */
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

    private:

        const FOptionalOps*     Ops = nullptr;
        FProperty*              Inner = nullptr;
    };
}
