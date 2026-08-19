#pragma once


namespace Lumina::Concept
{
    template<typename T, typename U>
    concept TSameAs = std::is_same_v<T, U> && std::is_same_v<U, T>;
}
