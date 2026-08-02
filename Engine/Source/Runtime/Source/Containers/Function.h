#pragma once

#include "Any.h"
#include "Array.h"
#include "Name.h"
#include "EASTL/internal/function.h"
#include <functional>


namespace Lumina
{
    template<typename T> using TFunction            = eastl::function<T>;
    template<typename T> using TMoveOnlyFunction    = std::move_only_function<T>;
    
}
