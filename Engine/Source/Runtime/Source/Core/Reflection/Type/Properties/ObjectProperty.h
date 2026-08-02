#pragma once
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class FObjectProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::Object)

        FObjectProperty(const FFieldOwner& InOwner, const FObjectPropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            ObjectClass = Params->ClassFunc();
            SetElementSize(sizeof(void*));
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Send the referenced object's stable GUID and resolve it on the receiver
        // (assets have deterministic/persistent GUIDs identical on both ends).
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        // Assign through TObjectPtr; a raw memcpy (base impl) would skip the strong-ref
        // add/release and corrupt the refcount (crash on a later release).
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        RUNTIME_API CClass* GetPropertyClass() const { return ObjectClass; }
        
    private:
        
        CClass* ObjectClass = nullptr;
    };
}
