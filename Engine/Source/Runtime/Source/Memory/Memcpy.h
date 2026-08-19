#pragma once

#include <cstring>
#include <immintrin.h>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define LUMINA_MEMCPY_X86 1
#else
    #define LUMINA_MEMCPY_X86 0
#endif

#if LUMINA_MEMCPY_X86 && defined(__AVX__)
    #define LUMINA_MEMCPY_VECTOR_WIDTH 32
#elif LUMINA_MEMCPY_X86
    #define LUMINA_MEMCPY_VECTOR_WIDTH 16
#else
    #define LUMINA_MEMCPY_VECTOR_WIDTH 0
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define LUMINA_IS_CONSTANT(X) __builtin_constant_p(X)
#else
    #define LUMINA_IS_CONSTANT(X) false
#endif

namespace Lumina::Memory
{
#if LUMINA_MEMCPY_VECTOR_WIDTH != 0

    // Copy kernels derived from DPDK rte_memcpy.h, BSD-3-Clause, see ThirdParty/DPDK/LICENSE.
    namespace Detail
    {
        // Measured crossovers: rep-movsb absorbs misalignment in microcode, so unaligned gives up sooner.
        inline constexpr size_t kAlignedVectorLimit = 4096;
        inline constexpr size_t kGenericVectorLimit = 1024;

        inline constexpr uintptr_t kAlignmentMask = LUMINA_MEMCPY_VECTOR_WIDTH - 1;

        FORCEINLINE void Move15OrLess(uint8* RESTRICT Dest, const uint8* RESTRICT Src, size_t Size)
        {
            if (Size & 8)
            {
                std::memcpy(Dest, Src, 8);
                Dest += 8;
                Src  += 8;
            }
            if (Size & 4)
            {
                std::memcpy(Dest, Src, 4);
                Dest += 4;
                Src  += 4;
            }
            if (Size & 2)
            {
                std::memcpy(Dest, Src, 2);
                Dest += 2;
                Src  += 2;
            }
            if (Size & 1)
            {
                *Dest = *Src;
            }
        }

        FORCEINLINE void Move16(uint8* RESTRICT Dest, const uint8* RESTRICT Src)
        {
            const __m128i Block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Src));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(Dest), Block);
        }

        FORCEINLINE void Move32(uint8* RESTRICT Dest, const uint8* RESTRICT Src)
        {
        #if LUMINA_MEMCPY_VECTOR_WIDTH == 32
            const __m256i Block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Src));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(Dest), Block);
        #else
            Move16(Dest + 0 * 16, Src + 0 * 16);
            Move16(Dest + 1 * 16, Src + 1 * 16);
        #endif
        }

        FORCEINLINE void Move64(uint8* RESTRICT Dest, const uint8* RESTRICT Src)
        {
            Move32(Dest + 0 * 32, Src + 0 * 32);
            Move32(Dest + 1 * 32, Src + 1 * 32);
        }

        FORCEINLINE void Move128(uint8* RESTRICT Dest, const uint8* RESTRICT Src)
        {
            Move64(Dest + 0 * 64, Src + 0 * 64);
            Move64(Dest + 1 * 64, Src + 1 * 64);
        }

        // Loads are issued together so the stores are not serialized behind each load's latency.
        FORCEINLINE void Move128Blocks(uint8* RESTRICT Dest, const uint8* RESTRICT Src, size_t Size)
        {
            while (Size >= 128)
            {
            #if LUMINA_MEMCPY_VECTOR_WIDTH == 32
                const __m256i A = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Src + 0 * 32));
                const __m256i B = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Src + 1 * 32));
                const __m256i C = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Src + 2 * 32));
                const __m256i D = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Src + 3 * 32));

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(Dest + 0 * 32), A);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(Dest + 1 * 32), B);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(Dest + 2 * 32), C);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(Dest + 3 * 32), D);
            #else
                Move64(Dest + 0 * 64, Src + 0 * 64);
                Move64(Dest + 1 * 64, Src + 1 * 64);
            #endif

                Src  += 128;
                Dest += 128;
                Size -= 128;
            }
        }

        // Trailing bytes of an above-64 copy; the final store overlaps what was already written.
        FORCEINLINE void CopyTailUpTo127(uint8* RESTRICT Dest, const uint8* RESTRICT Src, size_t Size)
        {
            if (Size >= 64)
            {
                Move64(Dest, Src);
                Dest += 64;
                Src  += 64;
                Size -= 64;
            }

            if (Size > 32)
            {
                Move32(Dest, Src);
                Move32(Dest + Size - 32, Src + Size - 32);
                return;
            }

            if (Size > 0)
            {
                Move32(Dest + Size - 32, Src + Size - 32);
            }
        }

        FORCEINLINE void CopyAlignedAbove64(uint8* RESTRICT Dest, const uint8* RESTRICT Src, size_t Size)
        {
            for (; Size > 64; Size -= 64)
            {
                Move64(Dest, Src);
                Dest += 64;
                Src  += 64;
            }

            Move64(Dest + Size - 64, Src + Size - 64);
        }

        FORCEINLINE void CopyGenericAbove64(uint8* RESTRICT Dest, const uint8* RESTRICT Src, size_t Size)
        {
            if (Size <= 256)
            {
                if (Size >= 128)
                {
                    Move128(Dest, Src);
                    Dest += 128;
                    Src  += 128;
                    Size -= 128;
                }

                CopyTailUpTo127(Dest, Src, Size);
                return;
            }

            // Aligning the destination is what makes the block loop worth entering at all.
            const size_t Misaligned = reinterpret_cast<uintptr_t>(Dest) & kAlignmentMask;
            if (Misaligned != 0)
            {
                const size_t Head = LUMINA_MEMCPY_VECTOR_WIDTH - Misaligned;
                Move32(Dest, Src);
                Dest += Head;
                Src  += Head;
                Size -= Head;
            }

            Move128Blocks(Dest, Src, Size);

            const size_t Bulk = Size & ~static_cast<size_t>(127);
            Dest += Bulk;
            Src  += Bulk;
            Size -= Bulk;

            CopyTailUpTo127(Dest, Src, Size);
        }

        FORCEINLINE void CopyDispatch(void* RESTRICT Destination, const void* RESTRICT Source, size_t Size)
        {
            uint8* RESTRICT       Dest = static_cast<uint8*>(Destination);
            const uint8* RESTRICT Src  = static_cast<const uint8*>(Source);

            if (Size < 16)
            {
                Move15OrLess(Dest, Src, Size);
                return;
            }

            if (Size <= 32)
            {
                Move16(Dest, Src);
                if (LUMINA_IS_CONSTANT(Size) && Size == 16)
                {
                    return;
                }
                Move16(Dest + Size - 16, Src + Size - 16);
                return;
            }

            if (Size <= 64)
            {
                Move32(Dest, Src);
                if (LUMINA_IS_CONSTANT(Size) && Size == 32)
                {
                    return;
                }
                Move32(Dest + Size - 32, Src + Size - 32);
                return;
            }

            // One overlapping pair beats the block path here: same store count, no size-dependent branching.
            if (Size <= 128)
            {
                Move64(Dest, Src);
                Move64(Dest + Size - 64, Src + Size - 64);
                return;
            }

            const uintptr_t Combined = reinterpret_cast<uintptr_t>(Dest) | reinterpret_cast<uintptr_t>(Src);

            if ((Combined & kAlignmentMask) == 0)
            {
                if (Size <= kAlignedVectorLimit)
                {
                    CopyAlignedAbove64(Dest, Src, Size);
                    return;
                }
            }
            else if (Size <= kGenericVectorLimit)
            {
                CopyGenericAbove64(Dest, Src, Size);
                return;
            }

            std::memcpy(Dest, Src, Size);
        }
    }

    FORCEINLINE void Memcpy(void* RESTRICT Destination, const void* RESTRICT Source, size_t SrcSize)
    {
        Detail::CopyDispatch(Destination, Source, SrcSize);
    }

    FORCEINLINE void Memcpy(void* RESTRICT Destination, void* RESTRICT Source, size_t SrcSize)
    {
        Detail::CopyDispatch(Destination, Source, SrcSize);
    }

#else

    FORCEINLINE void Memcpy(void* RESTRICT Destination, const void* RESTRICT Source, size_t SrcSize)
    {
        std::memcpy(Destination, Source, SrcSize);
    }

    FORCEINLINE void Memcpy(void* RESTRICT Destination, void* RESTRICT Source, size_t SrcSize)
    {
        std::memcpy(Destination, Source, SrcSize);
    }

#endif

    FORCEINLINE void MemcpyToWriteCombined(void* RESTRICT Destination, const void* RESTRICT Source, size_t Size)
    {
        auto*       Dest = static_cast<unsigned char*>(Destination);
        const auto* Src  = static_cast<const unsigned char*>(Source);

        const size_t Misaligned = reinterpret_cast<uintptr_t>(Dest) & 15u;
        if (Misaligned != 0)
        {
            const size_t Head = (16u - Misaligned) < Size ? (16u - Misaligned) : Size;
            std::memcpy(Dest, Src, Head);
            Dest += Head;
            Src  += Head;
            Size -= Head;
        }

        // 64 bytes per iteration, one full WC line, so each is combined and flushed once.
        for (size_t Lines = Size / 64; Lines != 0; --Lines)
        {
            const __m128i A = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Src) + 0);
            const __m128i B = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Src) + 1);
            const __m128i C = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Src) + 2);
            const __m128i D = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Src) + 3);

            _mm_stream_si128(reinterpret_cast<__m128i*>(Dest) + 0, A);
            _mm_stream_si128(reinterpret_cast<__m128i*>(Dest) + 1, B);
            _mm_stream_si128(reinterpret_cast<__m128i*>(Dest) + 2, C);
            _mm_stream_si128(reinterpret_cast<__m128i*>(Dest) + 3, D);

            Dest += 64;
            Src  += 64;
        }

        if (const size_t Tail = Size & 63u)
        {
            std::memcpy(Dest, Src, Tail);
        }

        _mm_sfence();
    }
}
