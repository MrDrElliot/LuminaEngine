#pragma once

#include "Core/Object/ObjectReferenceProvider.h"

namespace Lumina
{
    namespace ECS { class FRegistry; }

    /** Every reflected component in one registry, so a holder of a registry can expose what it points at. */
    RUNTIME_API void VisitRegistryObjectReferences(ECS::FRegistry& Registry, FObjectReferenceVisitor::FSlotFunc Func);

    /**
     * Every reflected component in every live world's registry.
     *
     * A prefab's own registries are declared by CPrefab itself, since it is a CObject and can. Generic over the
     * component's CStruct rather than over a list of component types, so a component that gains a
     * TStrongObjectPtr takes part without anything here changing.
     */
    class FECSObjectReferenceProvider final : public IObjectReferenceProvider
    {
    public:

        RUNTIME_API static void Register();

        const char* GetReferenceProviderName() const override { return "ECS component storages"; }

        RUNTIME_API void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;
    };
}
