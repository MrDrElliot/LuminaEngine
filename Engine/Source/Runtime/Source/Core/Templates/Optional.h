#pragma once
#include "Containers/Optional.h"

namespace Lumina
{
    template<typename T> using TOptional = Containers::TOptional<T>;

    using Containers::NullOpt;
    using Containers::MakeOptional;
}
