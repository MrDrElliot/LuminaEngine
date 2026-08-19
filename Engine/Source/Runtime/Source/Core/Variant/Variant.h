#pragma once
#include "Containers/Variant.h"

namespace Lumina
{
    template<typename... Args> using TVariant = Containers::TVariant<Args...>;

    using Containers::Visit;
    using Containers::HoldsAlternative;
}
