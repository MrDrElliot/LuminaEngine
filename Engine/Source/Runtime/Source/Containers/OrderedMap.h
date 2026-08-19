#pragma once

#include <map>

namespace Lumina
{
    // Ordered lookup stays on the standard library: nothing here needs a bespoke red-black tree.
    template <typename K, typename V>
    using TOrderedMap = std::map<K, V>;
}
