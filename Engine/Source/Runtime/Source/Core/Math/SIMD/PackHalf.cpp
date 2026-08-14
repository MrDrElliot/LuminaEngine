#include "PackHalf.h"

#include "SIMDConfig.h"
#include "Core/Math/Packing.h"

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <cpuid.h>
#endif

namespace Lumina::SIMD
{
    namespace
    {
        bool DetectF16C()
        {
            int Info[4] = {};
        #if defined(_MSC_VER)
            __cpuid(Info, 1);
        #else
            __cpuid(1, Info[0], Info[1], Info[2], Info[3]);
        #endif
            return (Info[2] & (1 << 29)) != 0;
        }

        void PackScalar(const float* Src, uint32* Dst, uint32 Count)
        {
            for (uint32 i = 0; i < Count; ++i)
            {
                Dst[i] = Math::PackHalf2x16(TVec<float, 2>(Src[i * 2], Src[i * 2 + 1]));
            }
        }

    #if !defined(_MSC_VER)
        __attribute__((target("f16c,avx")))
    #endif
        void PackF16C(const float* Src, uint32* Dst, uint32 Count)
        {
            // One xy pair per output, so eight source floats convert to four packed results.
            uint32 i = 0;
            for (; i + 4 <= Count; i += 4)
            {
                const __m256  Pairs = _mm256_loadu_ps(Src + i * 2);
                const __m128i Halves = _mm256_cvtps_ph(Pairs, _MM_FROUND_TO_NEAREST_INT);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(Dst + i), Halves);
            }

            PackScalar(Src + i * 2, Dst + i, Count - i);
        }
    }

    bool HasF16C()
    {
        static const bool bSupported = DetectF16C();
        return bSupported;
    }

    void PackHalf2x16Array(const float* Src, uint32* Dst, uint32 Count)
    {
        if (HasF16C())
        {
            PackF16C(Src, Dst, Count);
            return;
        }

        PackScalar(Src, Dst, Count);
    }
}
