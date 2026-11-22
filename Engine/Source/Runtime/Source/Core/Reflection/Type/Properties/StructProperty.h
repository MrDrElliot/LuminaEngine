#pragma once
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class FStructProperty : public FProperty
    {
    public:
        explicit FStructProperty(const FPropertyDesc& InDesc)
            : FProperty(InDesc)
        {
            CStruct* InternalStruct = InDesc.Type->GetStruct();
            ASSERT(InternalStruct);
            SetStruct(InternalStruct);
            SetElementSize(Struct->GetAlignedSize());
        }

        void Serialize(FArchive& Ar, void* Value) override;
        void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        // Nested struct: defer to the struct (its net serializer if it has one, else recurse fields).
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;

        void SetStruct(CStruct* InStruct) { Struct = InStruct; }
        CStruct* GetStruct() const { return Struct; }

        // Uses FStructOps when the struct opted in, otherwise recurses through the property list.
        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // Defers to the struct, which alone knows whether its members own memory, and a minted script struct seeds its C# defaults here.
        void ConstructValue(void* Value) const override { if (Struct) { Struct->InitializeStruct(Value); } }
        void DestructValue(void* Value) const override  { if (Struct) { Struct->DestroyStruct(Value); } }
        bool OwnsStorage() const override { return Struct != nullptr && Struct->RequiresValueLifecycle(); }
        void RelocateValue(void* Dest, void* Source) const override { if (Struct) { Struct->RelocateStruct(Dest, Source); } }


        CStruct* Struct = nullptr;
    
    };
}
