#pragma once
#include "EASTL/variant.h"

namespace Lumina
{
    template<typename... Args> using TVariant = eastl::variant<Args...>;
    
    
}
