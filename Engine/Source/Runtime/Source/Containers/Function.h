#pragma once

#include <functional>

#include "Any.h"
#include "Name.h"

namespace Lumina
{
    template<typename T> using TFunction            = std::function<T>;
    template<typename T> using TMoveOnlyFunction    = std::move_only_function<T>;
}
