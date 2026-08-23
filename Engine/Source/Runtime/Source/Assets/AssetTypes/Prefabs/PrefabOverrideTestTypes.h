#pragma once

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
}
