#pragma once

#include <set>

namespace Lumina
{
    // Ordered lookup stays on the standard library: nothing here needs a bespoke red-black tree.
    template <typename T>
    using TSet = std::set<T>;
}
