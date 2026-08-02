#pragma once

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Containers/ContainerOps.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{


    class FArrayProperty : public FProperty
    {
    public:
        FArrayProperty(const FFieldOwner& InOwner, const FArrayPropertyParams* Params)
            : FProperty(InOwner, Params)
        {
            Ops = Params->GetOpsFn ? Params->GetOpsFn() : nullptr;
        }
        
        void AddProperty(FProperty* Property) override { Inner.reset(Property); }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Tight: uint32 count + each element via the element property's own NetSerialize.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        /** Per-element compare via Inner; CopyCompleteValue resizes Dst then element-copies. */
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        FProperty* GetInternalProperty() const { return Inner.get(); }
        
        SIZE_T GetNum(const void* InContainer) const
        {
            return Ops->Size(InContainer);
        }

        void PushBack(void* InContainer, const void* InValue) const
        {
            Ops->PushBack(InContainer, InValue);
        }

        void RemoveAt(void* InContainer, size_t Index) const
        {
            Ops->RemoveAt(InContainer, Index);
        }

        void Clear(void* InContainer) const
        {
            Ops->Clear(InContainer);
        }

        // The vector is contiguous, so element i is Data() + i * ElementSize (no per-element fn-ptr needed).
        void* GetAt(void* InContainer, size_t Index) const
        {
            return static_cast<uint8*>(Ops->Data(InContainer)) + Index * Ops->ElementSize;
        }

        void Resize(void* InContainer, size_t Size) const
        {
            Ops->Resize(InContainer, Size);
        }

        void Reserve(void* InContainer, size_t Size) const
        {
            Ops->Reserve(InContainer, Size);
        }

        void Swap(void* InContainer, size_t LHS, size_t RHS) const
        {
            Ops->Swap(InContainer, LHS, RHS);
        }

        template<typename T = void, typename TFunc>
        void ForEach(void* InContainer, TFunc&& Func) const
        {
            SIZE_T Num = GetNum(InContainer);
            for (SIZE_T i = 0; i < Num; ++i)
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

        TUniquePtr<FProperty>   Inner;

    };
}
