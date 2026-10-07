#include "PixelOps.h"

#include "SIMDConfig.h"

namespace Lumina::SIMD
{
    namespace
    {
        // A wide step reads a full 32 bytes but consumes 24, so the guard keeps the read inside the source.
        constexpr size_t kRGBA8PixelsPerStep  = 8;
        constexpr size_t kRGBA8GuardPixels    = 11;
        constexpr size_t kRGBA16PixelsPerStep = 4;
        constexpr size_t kRGBA16GuardPixels   = 6;

        // Byte shuffles stay inside each 128-bit lane, so source dwords 3 to 5 move up to the high lane first.
        FORCEINLINE __m256i SplitTriplesAcrossLanes(const void* Src)
        {
            const __m256i Raw = _mm256_loadu_si256(static_cast<const __m256i*>(Src));
            return _mm256_permutevar8x32_epi32(Raw, _mm256_setr_epi32(0, 1, 2, 0, 3, 4, 5, 0));
        }

        void ExpandTail8(const uint8* Src, uint8* Dst, size_t From, size_t PixelCount)
        {
            for (size_t i = From; i < PixelCount; ++i)
            {
                Dst[i * 4 + 0] = Src[i * 3 + 0];
                Dst[i * 4 + 1] = Src[i * 3 + 1];
                Dst[i * 4 + 2] = Src[i * 3 + 2];
                Dst[i * 4 + 3] = 0xFFu;
            }
        }

        void ExpandTail16(const uint16* Src, uint16* Dst, size_t From, size_t PixelCount)
        {
            for (size_t i = From; i < PixelCount; ++i)
            {
                Dst[i * 4 + 0] = Src[i * 3 + 0];
                Dst[i * 4 + 1] = Src[i * 3 + 1];
                Dst[i * 4 + 2] = Src[i * 3 + 2];
                Dst[i * 4 + 3] = 0xFFFFu;
            }
        }
    }

    void ExpandRGBToRGBA8(const uint8* Src, uint8* Dst, size_t PixelCount)
    {
        // 0x80 zeroes that destination byte, leaving the alpha lane for the bitwise or below to fill.
        const __m256i Shuffle = _mm256_setr_epi8(0, 1, 2, (char)0x80, 3, 4, 5, (char)0x80,
                                                 6, 7, 8, (char)0x80, 9, 10, 11, (char)0x80,
                                                 0, 1, 2, (char)0x80, 3, 4, 5, (char)0x80,
                                                 6, 7, 8, (char)0x80, 9, 10, 11, (char)0x80);
        const __m256i Alpha = _mm256_set1_epi32((int)0xFF000000u);

        size_t i = 0;
        for (; i + kRGBA8GuardPixels <= PixelCount; i += kRGBA8PixelsPerStep)
        {
            const __m256i Quads = _mm256_or_si256(_mm256_shuffle_epi8(SplitTriplesAcrossLanes(Src + i * 3), Shuffle), Alpha);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(Dst + i * 4), Quads);
        }

        ExpandTail8(Src, Dst, i, PixelCount);
    }

    void ExpandRGBToRGBA16(const uint16* Src, uint16* Dst, size_t PixelCount)
    {
        const __m256i Shuffle = _mm256_setr_epi8(0, 1, 2, 3, 4, 5, (char)0x80, (char)0x80,
                                                 6, 7, 8, 9, 10, 11, (char)0x80, (char)0x80,
                                                 0, 1, 2, 3, 4, 5, (char)0x80, (char)0x80,
                                                 6, 7, 8, 9, 10, 11, (char)0x80, (char)0x80);
        const __m256i Alpha = _mm256_set1_epi64x((long long)0xFFFF000000000000ull);

        size_t i = 0;
        for (; i + kRGBA16GuardPixels <= PixelCount; i += kRGBA16PixelsPerStep)
        {
            const __m256i Quads = _mm256_or_si256(_mm256_shuffle_epi8(SplitTriplesAcrossLanes(Src + i * 3), Shuffle), Alpha);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(Dst + i * 4), Quads);
        }

        ExpandTail16(Src, Dst, i, PixelCount);
    }
}
