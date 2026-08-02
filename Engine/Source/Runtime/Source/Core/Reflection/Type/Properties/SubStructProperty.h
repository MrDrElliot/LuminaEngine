#pragma once
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class CStruct;

    // Backs TSubStructOf<T>: a property whose value is a CStruct* constrained to MetaStruct (T) or a
    // derived struct. The struct analog of FClassProperty; serialized by struct name.
    class FSubStructProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::SubStruct)

        FSubStructProperty(const FFieldOwner& InOwner, const FSubStructPropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            MetaStruct = Params->StructFunc();
            SetElementSize(sizeof(void*));
        }

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // The base struct assignable values must derive from (the T in TSubStructOf<T>).
        RUNTIME_API CStruct* GetMetaStruct() const { return MetaStruct; }

    private:

        CStruct* MetaStruct = nullptr;
    };
}
