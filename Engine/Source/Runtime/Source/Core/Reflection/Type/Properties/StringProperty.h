#pragma once
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Memory/Construct.h"

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

        // Owns heap memory, so zeroed bytes are NOT a valid value: a memzeroed FString reads as a plausible
        // empty string and corrupts on the first assignment.
        void ConstructValue(void* Value) const override { Memory::ConstructAt(static_cast<FString*>(Value)); }
        bool OwnsStorage() const override { return true; }
        void DestructValue(void* Value) const override  { static_cast<FString*>(Value)->~FString(); }
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
