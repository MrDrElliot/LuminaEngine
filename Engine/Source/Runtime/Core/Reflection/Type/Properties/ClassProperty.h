#pragma once
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class CClass;

    // Backs TSubclassOf<T>: a property whose value is a CClass* constrained to MetaClass (T) or a
    // subclass. Stores a single CClass* (the type is a permanent singleton, so no refcount/copy
    // handling is needed); serialized by class name and re-resolved on load.
    class FClassProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::Class)

        FClassProperty(const FFieldOwner& InOwner, const FClassPropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            MetaClass = Params->ClassFunc();
            SetElementSize(sizeof(void*));
        }

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // The base class assignable values must derive from (the T in TSubclassOf<T>).
        RUNTIME_API CClass* GetMetaClass() const { return MetaClass; }

    private:

        // Resolves a serialized class name to a CClass*, enforcing the MetaClass (TSubclassOf<T>) constraint that
        // the raw-pointer write in Serialize/SerializeItem otherwise bypasses. Returns null for none / unknown /
        // not-a-subclass so a malformed or stale config can never yield a wrong-typed class.
        CClass* ResolveSerializedClass(const FName& ClassName) const;

        CClass* MetaClass = nullptr;
    };
}
