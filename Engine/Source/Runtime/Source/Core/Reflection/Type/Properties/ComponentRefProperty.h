#pragma once
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class CStruct;

    // Backs TComponentRef<T>, a reference to a component of the owner's entity that is found again at every attach and never saved.
    class LUMINA_VISIBLE_TYPE FComponentRefProperty : public FProperty
    {
    public:
        RUNTIME_API explicit FComponentRefProperty(const FPropertyDesc& InDesc);

        // Runtime state, so two of them never differ in a way an override, an undo step or a reset arrow should see.
        bool Identical(const void* ValueA, const void* ValueB) const override { return true; }

        // A raw pointer is meaningless on another peer, so a reference never travels.
        void NetSerialize(FNetArchive& Ar, void* Value) override {}

        RUNTIME_API FString ToString(const void* Data) const override;

        // The component type the reference names.
        NODISCARD CStruct* GetComponentStruct() const { return ComponentStruct; }

    private:

        CStruct* ComponentStruct = nullptr;
    };
}
