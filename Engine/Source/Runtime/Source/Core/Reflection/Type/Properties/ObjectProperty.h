#pragma once
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class FObjectProperty : public FProperty
    {
    public:
        explicit FObjectProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            ObjectClass = InDesc.Type->GetClass();
            SetElementSize(sizeof(TStrongObjectPtr<CObject>));
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Sends the stable GUID and resolves it on the receiver, since assets share GUIDs on both ends.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        // Assigns through TStrongObjectPtr because a raw memcpy would skip the add and release and corrupt the refcount.
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // A zeroed TStrongObjectPtr is a valid null, but destruction must release the strong ref or the object never dies.
        RUNTIME_API void DestructValue(void* Value) const override;
        bool OwnsStorage() const override { return true; }

        RUNTIME_API CClass* GetPropertyClass() const { return ObjectClass; }
        
    private:
        
        CClass* ObjectClass = nullptr;
    };
}
