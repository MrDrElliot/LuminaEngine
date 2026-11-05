#pragma once

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Containers/ContainerOps.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{


    class FArrayProperty : public FProperty
    {
    public:
        explicit FArrayProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            Ops = InDesc.Type->GetVectorOps();
            SetElementSize(Ops != nullptr ? Ops->ContainerSize : 0);
        }
        
        void AddProperty(FProperty* Property) override { Inner = Property; }

        // Numbers, bools and enums, which a byte compare and a byte copy handle exactly.
        bool HasPlainElements() const;

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Tight: uint32 count + each element via the element property's own NetSerialize.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        /** Per-element compare via Inner; CopyCompleteValue resizes Dst then element-copies. */
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // The container owns heap memory, so zeroed bytes are not a valid value. Routed through the ops table
        // rather than a type test, so a TVector<T> and a script runtime array are handled by the same code.
        void ConstructValue(void* Value) const override { if (Ops && Ops->ConstructContainer) { Ops->ConstructContainer(Value, Ops->ContainerContext); } }
        void DestructValue(void* Value) const override  { if (Ops && Ops->DestructContainer)  { Ops->DestructContainer(Value, Ops->ContainerContext); } }
        bool OwnsStorage() const override { return Ops != nullptr && Ops->ConstructContainer != nullptr; }

        FProperty* GetInternalProperty() const { return Inner; }

        /** The element ops table. Exposed so C# can build a Lumina.TVector<T> view over any array property. */
        const FVectorOps* GetOps() const { return Ops; }
        
        int32 Num(const void* InContainer) const
        {
            return Ops->Num(InContainer);
        }

        void Add(void* InContainer, const void* InValue) const
        {
            Ops->Add(InContainer, InValue);
        }

        void RemoveAt(void* InContainer, int32 Index) const
        {
            Ops->RemoveAt(InContainer, Index);
        }

        void Reset(void* InContainer) const
        {
            Ops->Reset(InContainer);
        }

        // The vector is contiguous, so element i is GetData() + i * ElementSize (no per-element fn-ptr needed).
        void* GetAt(void* InContainer, int32 Index) const
        {
            return static_cast<uint8*>(Ops->GetData(InContainer)) + static_cast<int64>(Index) * Ops->ElementSize;
        }

        void SetNum(void* InContainer, int32 NewNum) const
        {
            Ops->SetNum(InContainer, NewNum);
        }

        void Reserve(void* InContainer, int32 Number) const
        {
            Ops->Reserve(InContainer, Number);
        }

        void Swap(void* InContainer, int32 A, int32 B) const
        {
            Ops->Swap(InContainer, A, B);
        }

        template<typename T = void, typename TFunc>
        void ForEach(void* InContainer, TFunc&& Func) const
        {
            const int32 Count = Num(InContainer);
            for (int32 i = 0; i < Count; ++i)
            {
                if constexpr (std::is_same_v<T, void>)
                {
                    void* Elem = GetAt(InContainer, i);
                    Func(Elem, i);
                }
                else
                {
                    T* Elem = static_cast<T*>(GetAt(InContainer, i));
                    Func(Elem, i);
                }
            }
        }
        
    private:

        const FVectorOps*       Ops = nullptr;

        // Placed in the owning type's arena alongside this property, which is what frees it.
        FProperty*              Inner = nullptr;

    };
}
