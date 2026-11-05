#pragma once

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Containers/ContainerOps.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    // Reflected THashMap<K,V>: the associative analogue of FArrayProperty. Holds a Key and a Value inner property
    // and operates on the container through a type-erased FMapOps table.
    class LUMINA_VISIBLE_TYPE FMapProperty : public FProperty
    {
    public:
        explicit FMapProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            Ops = InDesc.Type->GetMapOps();
            SetElementSize(Ops != nullptr ? Ops->ContainerSize : 0);
        }

        // ConstructProperty attaches the key before the value, in the order the map's type lists its inners.
        void AddProperty(FProperty* Property) override
        {
            if (!KeyProperty) { KeyProperty = Property; }
            else              { ValueProperty = Property; }
        }

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        /** Order-independent: every key in A must exist in B with an Identical value (counts already match). */
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // See FArrayProperty: the container owns heap memory, and the ops table is what makes this work for a
        // compile-time THashMap and a script runtime map alike.
        void ConstructValue(void* Value) const override { if (Ops && Ops->ConstructContainer) { Ops->ConstructContainer(Value, Ops->ContainerContext); } }
        void DestructValue(void* Value) const override  { if (Ops && Ops->DestructContainer)  { Ops->DestructContainer(Value, Ops->ContainerContext); } }
        bool OwnsStorage() const override { return Ops != nullptr && Ops->ConstructContainer != nullptr; }

        /** The key/value ops table. Exposed so C# can build a Lumina.THashMap<K,V> view over any map property. */
        const FMapOps* GetOps() const { return Ops; }

        FProperty* GetKeyProperty()   const { return KeyProperty; }
        FProperty* GetValueProperty() const { return ValueProperty; }

        int32 Num(const void* InContainer) const { return Ops->Num(InContainer); }

        // Insert-or-assign; InValue null => default-constructed value. Returns the stored value slot.
        void*  Add(void* InContainer, const void* InKey, const void* InValue) const { return Ops->Add(InContainer, InKey, InValue); }
        void*  Find(void* InContainer, const void* InKey) const { return Ops->Find(InContainer, InKey); }
        bool   Remove(void* InContainer, const void* InKey) const { return Ops->Remove(InContainer, InKey); }
        void   Reset(void* InContainer) const { Ops->Reset(InContainer); }
        void   Reserve(void* InContainer, int32 Number) const { Ops->Reserve(InContainer, Number); }

        uint32 GetKeySize()   const { return Ops->KeySize; }

        // Index-based access into the mutation-stable iteration order, re-resolved each call, so an editor can
        // address entries across container reallocations.
        const void* GetKeyAt(void* InContainer, int32 Index)   const { const void* K = nullptr; Ops->At(InContainer, Index, &K, nullptr); return K; }
        void*       GetValueAt(void* InContainer, int32 Index) const { void* V = nullptr; Ops->At(InContainer, Index, nullptr, &V); return V; }

        // Placement construct/destruct a scratch key, for building a temp key to Add/Find with.
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

        FProperty*              KeyProperty = nullptr;
        FProperty*              ValueProperty = nullptr;
    };
}
