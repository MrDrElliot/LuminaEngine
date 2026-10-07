#include "PackHalf.h"

#include "SIMDConfig.h"
#include "Core/Math/Packing.h"

namespace Lumina::SIMD
{
    void PackHalf2x16Array(const float* Src, uint32* Dst, uint32 Count)
    {
        // One xy pair per output, so eight source floats convert to four packed results.
        uint32 i = 0;
        for (; i + 4 <= Count; i += 4)
        {
            const __m256  Pairs  = _mm256_loadu_ps(Src + i * 2);
            const __m128i Halves = _mm256_cvtps_ph(Pairs, _MM_FROUND_TO_NEAREST_INT);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(Dst + i), Halves);
        }

        for (; i < Count; ++i)
        {
            Dst[i] = Math::PackHalf2x16(TVec<float, 2>(Src[i * 2], Src[i * 2 + 1]));
        }
    }
}
