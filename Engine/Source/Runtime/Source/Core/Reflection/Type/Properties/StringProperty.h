#pragma once
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class FStringProperty : public FProperty
    {
    public:

        FStringProperty(const FFieldOwner& InOwner, const FPropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            SetElementSize(sizeof(FString));
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;
    };


    class FNameProperty : public FProperty
    {
    public:
        
        FNameProperty(FFieldOwner InOwner, const FPropertyParams* Params)
            :FProperty(InOwner, Params)
        {
            SetElementSize(sizeof(FName));
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Tight: a compact net index when the archive binds the name-index hooks (string exported once via
        // NameExport), else the raw string.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

    };

}
