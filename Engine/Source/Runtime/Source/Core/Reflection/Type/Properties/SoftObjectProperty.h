#pragma once
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    // FObjectProperty counterpart for soft refs; Serialize routes through FSoftObjectPath::operator<<
    // so the saver records a Soft ImportTable entry.
    class FSoftObjectProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::SoftObject)

        FSoftObjectProperty(const FFieldOwner& InOwner, const FSoftObjectPropertyParams* Params)
            : FProperty(InOwner, Params)
        {
            ObjectClass = Params->ClassFunc ? Params->ClassFunc() : nullptr;
            // Storage is FSoftObjectPath (path + GUID). TSoftObjectPtr<T> is
            // layout-identical (single FSoftObjectPath member, no vtable).
            SetElementSize(sizeof(FSoftObjectPath));
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Storage holds an FString; a raw memcpy (base impl) would share the heap buffer between
        // Dst and Src (double-free), and a byte memcmp would compare heap pointers + CachedGUID
        // instead of the path. Route through FSoftObjectPath's assignment / path-only equality.
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        RUNTIME_API CClass* GetPropertyClass() const { return ObjectClass; }

    private:

        CClass* ObjectClass = nullptr;
    };
}
