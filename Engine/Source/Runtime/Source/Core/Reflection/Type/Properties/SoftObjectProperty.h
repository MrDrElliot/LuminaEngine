#pragma once
#include "Core/Object/SoftObjectPtr.h"
#include "Memory/Construct.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    // Serialize routes through FSoftObjectPath::operator<< so the saver records a Soft ImportTable entry.
    class LUMINA_VISIBLE_TYPE FSoftObjectProperty : public FProperty
    {
    public:
        explicit FSoftObjectProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            ObjectClass = InDesc.Type->GetClass();
            // TSoftObjectPtr<T> is layout-identical to FSoftObjectPath.
            SetElementSize(sizeof(FSoftObjectPath));
        }

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Storage holds an FString, so a raw memcpy would double-free and a byte compare would test heap pointers instead of the path.
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // The path holds an FString, so zeroed bytes are not a valid value (see FStringProperty).
        void ConstructValue(void* Value) const override { Memory::ConstructAt(static_cast<FSoftObjectPath*>(Value)); }
        void DestructValue(void* Value) const override  { static_cast<FSoftObjectPath*>(Value)->~FSoftObjectPath(); }
        bool OwnsStorage() const override { return true; }

        RUNTIME_API CClass* GetPropertyClass() const { return ObjectClass; }

    private:

        CClass* ObjectClass = nullptr;
    };
}
