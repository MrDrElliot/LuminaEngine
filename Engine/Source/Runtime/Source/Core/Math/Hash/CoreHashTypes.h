#pragma once
#include "Hash.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Math/Matrix/Matrix.h"


namespace Lumina
{
    template<typename T, int N>
    NODISCARD inline uint64 GetTypeHash(const TVec<T, N>& V) noexcept
    {
        size_t Seed = 0;
        for (int i = 0; i < N; ++i)
        {
            Hash::HashCombine(Seed, V[i]);
        }
        return static_cast<uint64>(Seed);
    }

    template<typename T, int C, int R>
    NODISCARD inline uint64 GetTypeHash(const TMat<T, C, R>& M) noexcept
    {
        size_t Seed = 0;
        for (int c = 0; c < C; ++c)
        {
            for (int r = 0; r < R; ++r)
            {
                Hash::HashCombine(Seed, M[c][r]);
            }
        }
        return static_cast<uint64>(Seed);
    }
}
