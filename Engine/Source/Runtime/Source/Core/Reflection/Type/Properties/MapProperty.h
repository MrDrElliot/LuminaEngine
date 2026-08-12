#pragma once

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Containers/ContainerOps.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    // Reflected THashMap<K,V>: the associative analogue of FArrayProperty. Holds a Key and a Value inner property
    // and operates on the container through a type-erased FMapOps table.
    class FMapProperty : public FProperty
    {
    public:
        FMapProperty(const FFieldOwner& InOwner, const FMapPropertyParams* Params)
            : FProperty(InOwner, Params)
        {
            Ops = Params->GetOpsFn ? Params->GetOpsFn() : nullptr;
        }

        // Called by ConstructProperties for the two inners in a fixed order: the KEY first, then the VALUE (the
        // codegen emits PropPointers as [Value, Key, Map] and the backward ReadMore=2 walk consumes Key then
        // Value). Do NOT reorder these assignments -- the order is the ABI contract with the emitter.
        void AddProperty(FProperty* Property) override
        {
            if (!KeyProperty) { KeyProperty.reset(Property); }
            else              { ValueProperty.reset(Property); }
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        /** Order-independent: every key in A must exist in B with an Identical value (counts already match). */
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // See FArrayProperty: the container owns heap memory, and the ops table is what makes this work for a
        // compile-time THashMap and a script runtime map alike.
        void ConstructValue(void* Value) const override { if (Ops && Ops->ConstructContainer) { Ops->ConstructContainer(Value, Ops->ContainerContext); } }
        void DestructValue(void* Value) const override  { if (Ops && Ops->DestructContainer)  { Ops->DestructContainer(Value, Ops->ContainerContext); } }
        bool OwnsStorage() const override { return Ops != nullptr && Ops->ConstructContainer != nullptr; }

        FProperty* GetKeyProperty()   const { return KeyProperty.get(); }
        FProperty* GetValueProperty() const { return ValueProperty.get(); }

        SIZE_T GetNum(const void* InContainer) const { return Ops->Size(InContainer); }

        // Insert-or-assign; InValue null => default-constructed value. Returns the stored value slot.
        void*  Insert(void* InContainer, const void* InKey, const void* InValue) const { return Ops->Insert(InContainer, InKey, InValue); }
        void*  Find(void* InContainer, const void* InKey) const { return Ops->Find(InContainer, InKey); }
        bool   RemoveByKey(void* InContainer, const void* InKey) const { return Ops->RemoveByKey(InContainer, InKey); }
        void   Clear(void* InContainer) const { Ops->Clear(InContainer); }
        void   Reserve(void* InContainer, size_t Count) const { Ops->Reserve(InContainer, Count); }

        uint32 GetKeySize()   const { return Ops->KeySize; }
        uint32 GetValueSize() const { return Ops->ValueSize; }

        // Index-based access into the mutation-stable iteration order, re-resolved each call, so an editor can
        // address entries across container reallocations.
        const void* GetKeyAt(void* InContainer, size_t Index)   const { const void* K = nullptr; Ops->At(InContainer, Index, &K, nullptr); return K; }
        void*       GetValueAt(void* InContainer, size_t Index) const { void* V = nullptr; Ops->At(InContainer, Index, nullptr, &V); return V; }

        // Placement construct/destruct a scratch key, for building a temp key to Insert/Find with.
        void ConstructKey(void* InContainer, void* Dst) const { Ops->ConstructKey(InContainer, Dst); }
        void DestructKey(void* InContainer, void* Dst)  const { Ops->DestructKey(InContainer, Dst); }

        // Push-model iteration over live pairs. Func: void(const void* Key, void* Value).
        template<typename TFunc>
        void ForEach(const void* InContainer, const TFunc& Func) const
        {
            Ops->ForEach(InContainer,
                [](const void* Key, void* Val, void* User)
                {
                    (*static_cast<const TFunc*>(User))(Key, Val);
                }, const_cast<void*>(static_cast<const void*>(&Func)));
        }

    private:

        const FMapOps*          Ops = nullptr;

        TUniquePtr<FProperty>   KeyProperty;
        TUniquePtr<FProperty>   ValueProperty;
    };
}
