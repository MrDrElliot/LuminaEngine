#pragma once

#include <eastl/type_traits.h>

namespace Lumina::Concept
{
    template<typename T, typename U>
    concept TSameAs = eastl::is_same_v<T, U> && eastl::is_same_v<U, T>;
}