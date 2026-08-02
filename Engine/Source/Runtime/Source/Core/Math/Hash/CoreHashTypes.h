#pragma once
#include "Hash.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Math/Matrix/Matrix.h"


namespace eastl
{
    template<typename T, int N>
    struct hash<Lumina::TVec<T, N>>
    {
        size_t operator()(const Lumina::TVec<T, N>& v) const noexcept
        {
            size_t h = 0;
            for (int i = 0; i < N; ++i)
            {
                Lumina::Hash::HashCombine(h, v[i]);
            }
            return h;
        }
    };

    template<typename T, int C, int R>
    struct hash<Lumina::TMat<T, C, R>>
    {
        size_t operator()(const Lumina::TMat<T, C, R>& m) const noexcept
        {
            size_t h = 0;
            for (int c = 0; c < C; c++)
            {
                for (int r = 0; r < R; r++)
                {
                    Lumina::Hash::HashCombine(h, m[c][r]);
                }
            }
            return h;
        }
    };
}
