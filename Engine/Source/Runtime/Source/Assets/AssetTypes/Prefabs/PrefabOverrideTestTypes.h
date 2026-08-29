#pragma once

#include "World/ECS/Registry.h"
#include "Core/Object/ObjectMacros.h"
#include "PrefabOverrideTestTypes.generated.h"

namespace Lumina
{
    // Throwaway pair giving the override diff an inherited reflected property, which no component has yet.

    REFLECT()
    struct RUNTIME_API SPrefabLeafTestBase
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        int32 Inherited = 0;
    };

    REFLECT()
    struct RUNTIME_API SPrefabLeafTestDerived : public SPrefabLeafTestBase
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        int32 Own = 0;
    };

    // Gives the variant delta an entity handle to carry, which no component in the tree does yet.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SPrefabLinkTestComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Entity)
        ECS::FEntity Target = ECS::NullEntity;

        PROPERTY(Editable)
        int32 Value = 0;
    };
}
