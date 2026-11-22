#pragma once
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Memory/Construct.h"

namespace Lumina
{
    class FStringProperty : public FProperty
    {
    public:

        explicit FStringProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            SetElementSize(sizeof(FString));
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // Zeroed bytes are not a valid value, since a memzeroed FString reads as empty and corrupts on first assignment.
        void ConstructValue(void* Value) const override { Memory::ConstructAt(static_cast<FString*>(Value)); }
        bool OwnsStorage() const override { return true; }
        void DestructValue(void* Value) const override  { static_cast<FString*>(Value)->~FString(); }
    };


    class LUMINA_VISIBLE_TYPE FNameProperty : public FProperty
    {
    public:
        
        explicit FNameProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            SetElementSize(sizeof(FName));
        }

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Tight form uses a compact net index when the archive binds the name-index hooks, else the raw string.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

    };

}
