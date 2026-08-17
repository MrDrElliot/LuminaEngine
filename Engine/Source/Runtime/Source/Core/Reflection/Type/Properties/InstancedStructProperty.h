#pragma once
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class CStruct;

    // Backs TInstancedStruct<T>. Owns a heap struct instance whose concrete type is chosen at runtime
    // from MetaStruct or a derived struct, serialized inline as the chosen name plus its properties.
    class FInstancedStructProperty : public FProperty
    {
    public:
        DECLARE_FPROPERTY(EPropertyTypeFlags::InstancedStruct)

        FInstancedStructProperty(const FFieldOwner& InOwner, const FInstancedStructPropertyParams* Params);

        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;
        RUNTIME_API void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults) override;

        RUNTIME_API bool Identical(const void* ValueA, const void* ValueB) const override;
        RUNTIME_API void CopyCompleteValue(void* Dst, const void* Src) const override;

        // Owns a heap instance of whatever type was picked, so zeroed bytes are not a valid FInstancedStruct.
        RUNTIME_API void ConstructValue(void* Value) const override;
        RUNTIME_API void DestructValue(void* Value) const override;
        bool OwnsStorage() const override { return true; }

        // The base every owned instance must derive from: the T in TInstancedStruct<T>, or the type named
        // by PROPERTY(StructBase = "...") on a bare FInstancedStruct. Null means unconstrained.
        RUNTIME_API CStruct* GetMetaStruct() const;

    private:

        mutable CStruct* MetaStruct = nullptr;

        // StructBase names a type that may not exist yet at property construction, so resolve on first ask.
        mutable bool bResolvedStructBase = false;
    };
}
