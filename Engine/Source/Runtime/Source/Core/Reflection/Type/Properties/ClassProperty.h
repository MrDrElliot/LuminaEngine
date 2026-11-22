#pragma once
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class CClass;

    // Backs TSubclassOf<T>. Stores a bare CClass* since the type is a permanent singleton, serialized by class name.
    class LUMINA_VISIBLE_TYPE FClassProperty : public FProperty
    {
    public:
        explicit FClassProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            MetaClass = InDesc.Type->GetClass();
            SetElementSize(sizeof(void*));
        }

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // The base class assignable values must derive from (the T in TSubclassOf<T>).
        RUNTIME_API CClass* GetMetaClass() const { return MetaClass; }

    private:

        // Enforces the MetaClass constraint that the raw pointer write bypasses, returning null for unknown or non-subclass names.
        CClass* ResolveSerializedClass(const FName& ClassName) const;

        CClass* MetaClass = nullptr;
    };
}
