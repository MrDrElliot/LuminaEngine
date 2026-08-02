#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina::ECS
{
    enum class ETraits : uint8
    {
        None        = 0,
        System      = BIT(0),
        Component   = BIT(1),
        Event       = BIT(2),
    };
    
    ENUM_CLASS_FLAGS(ETraits);
}
