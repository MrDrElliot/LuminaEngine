#pragma once

#include "Core/Object/ObjectMacros.h"
#include "WorldTypes.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EWorldType : uint8
    {
        None,
        Game,
        Simulation,
        Editor,
    };
    
}
