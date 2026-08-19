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

// 256-bit integer compare is AVX2, unlike the 256-bit load/store the copy path uses.
#if LUMINA_MEMCPY_X86 && defined(__AVX2__)
    #define LUMINA_MEMCMP_VECTOR_WIDTH 32
#elif LUMINA_MEMCPY_X86
    #define LUMINA_MEMCMP_VECTOR_WIDTH 16
#else
    #define LUMINA_MEMCMP_VECTOR_WIDTH 0
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
        inline constexpr size_t kSetVectorLimit     = 4096;
        inline constexpr size_t kCompareVectorLimit = 4096;

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

        FORCEINLINE uint32 FirstSetBitIndex(uint32 Mask)
        {
        #if defined(_MSC_VER)
            unsigned long Index = 0;
            _BitScanForward(&Index, Mask);
            return static_cast<uint32>(Index);
        #else
            return static_cast<uint32>(__builtin_ctz(Mask));
        #endif
        }

        FORCEINLINE void Set15OrLess(uint8* RESTRICT Dest, uint64 Pattern, size_t Size)
        {
            if (Size & 8)
            {
                std::memcpy(Dest, &Pattern, 8);
                Dest += 8;
            }
            if (Size & 4)
            {
                std::memcpy(Dest, &Pattern, 4);
                Dest += 4;
            }
            if (Size & 2)
            {
                std::memcpy(Dest, &Pattern, 2);
                Dest += 2;
            }
            if (Size & 1)
            {
                *Dest = static_cast<uint8>(Pattern);
            }
        }

        FORCEINLINE void Set16(uint8* RESTRICT Dest, __m128i Block)
        {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(Dest), Block);
        }

        FORCEINLINE void SetBlocks128(uint8* RESTRICT Dest, __m128i Block, size_t Size)
        {
            while (Size >= 128)
            {
                Set16(Dest + 0 * 16, Block);
                Set16(Dest + 1 * 16, Block);
                Set16(Dest + 2 * 16, Block);
                Set16(Dest + 3 * 16, Block);
                Set16(Dest + 4 * 16, Block);
                Set16(Dest + 5 * 16, Block);
                Set16(Dest + 6 * 16, Block);
                Set16(Dest + 7 * 16, Block);

                Dest += 128;
                Size -= 128;
            }
        }

        FORCEINLINE void SetDispatch(void* RESTRICT Destination, int32 Value, size_t Size)
        {
            uint8* RESTRICT Dest = static_cast<uint8*>(Destination);

            const uint8  Byte    = static_cast<uint8>(Value);
            const uint64 Pattern = 0x0101010101010101ull * Byte;

            if (Size < 16)
            {
                Set15OrLess(Dest, Pattern, Size);
                return;
            }

            const __m128i Block = _mm_set1_epi8(static_cast<char>(Byte));

            if (Size <= 32)
            {
                Set16(Dest, Block);
                Set16(Dest + Size - 16, Block);
                return;
            }

            if (Size <= 64)
            {
                Set16(Dest + 0 * 16, Block);
                Set16(Dest + 1 * 16, Block);
                Set16(Dest + Size - 32, Block);
                Set16(Dest + Size - 16, Block);
                return;
            }

            if (Size > kSetVectorLimit)
            {
                std::memset(Dest, Value, Size);
                return;
            }

            // Store alignment is worth more than the duplicated head bytes once the block loop runs.
            const size_t Misaligned = reinterpret_cast<uintptr_t>(Dest) & 15u;
            uint8* Cursor = Dest;
            size_t Left   = Size;

            if (Misaligned != 0)
            {
                const size_t Head = 16 - Misaligned;
                Set16(Cursor, Block);
                Cursor += Head;
                Left   -= Head;
            }

            SetBlocks128(Cursor, Block, Left);

            const size_t Bulk = Left & ~static_cast<size_t>(127);
            Cursor += Bulk;
            Left   -= Bulk;

            while (Left >= 16)
            {
                Set16(Cursor, Block);
                Cursor += 16;
                Left   -= 16;
            }

            if (Left != 0)
            {
                Set16(Cursor + Left - 16, Block);
            }
        }

        FORCEINLINE int32 ByteDifference(const uint8* A, const uint8* B, uint32 Index)
        {
            return static_cast<int32>(A[Index]) - static_cast<int32>(B[Index]);
        }

        // Index of the first differing byte in the 16-byte window, or -1 when the window matches.
        FORCEINLINE int32 FirstDifference16(const uint8* A, const uint8* B)
        {
            const __m128i Left  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(A));
            const __m128i Right = _mm_loadu_si128(reinterpret_cast<const __m128i*>(B));
            const uint32 Equal  = static_cast<uint32>(_mm_movemask_epi8(_mm_cmpeq_epi8(Left, Right)));

            const uint32 Differs = (~Equal) & 0xFFFFu;
            return Differs == 0 ? -1 : static_cast<int32>(FirstSetBitIndex(Differs));
        }

    #if LUMINA_MEMCMP_VECTOR_WIDTH == 32
        FORCEINLINE int32 FirstDifference32(const uint8* A, const uint8* B)
        {
            const __m256i Left  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(A));
            const __m256i Right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(B));
            const uint32 Equal  = static_cast<uint32>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(Left, Right)));

            return Equal == 0xFFFFFFFFu ? -1 : static_cast<int32>(FirstSetBitIndex(~Equal));
        }
    #endif

        FORCEINLINE int32 CompareDispatch(const void* A, const void* B, size_t Size)
        {
            const uint8* Left  = static_cast<const uint8*>(A);
            const uint8* Right = static_cast<const uint8*>(B);

            if (Size < 16)
            {
                for (size_t Index = 0; Index < Size; ++Index)
                {
                    if (Left[Index] != Right[Index])
                    {
                        return ByteDifference(Left, Right, static_cast<uint32>(Index));
                    }
                }
                return 0;
            }

            if (Size <= 32)
            {
                const int32 Head = FirstDifference16(Left, Right);
                if (Head >= 0)
                {
                    return ByteDifference(Left, Right, static_cast<uint32>(Head));
                }

                const size_t Tail = Size - 16;
                const int32 Late  = FirstDifference16(Left + Tail, Right + Tail);
                return Late < 0 ? 0 : ByteDifference(Left + Tail, Right + Tail, static_cast<uint32>(Late));
            }

            if (Size > kCompareVectorLimit)
            {
                return std::memcmp(A, B, Size);
            }

            size_t Offset = 0;

        #if LUMINA_MEMCMP_VECTOR_WIDTH == 32
            for (; Offset + 32 <= Size; Offset += 32)
            {
                const int32 Found = FirstDifference32(Left + Offset, Right + Offset);
                if (Found >= 0)
                {
                    return ByteDifference(Left + Offset, Right + Offset, static_cast<uint32>(Found));
                }
            }
        #endif

            for (; Offset + 16 <= Size; Offset += 16)
            {
                const int32 Found = FirstDifference16(Left + Offset, Right + Offset);
                if (Found >= 0)
                {
                    return ByteDifference(Left + Offset, Right + Offset, static_cast<uint32>(Found));
                }
            }

            if (Offset < Size)
            {
                const size_t Tail = Size - 16;
                const int32 Found = FirstDifference16(Left + Tail, Right + Tail);
                if (Found >= 0)
                {
                    return ByteDifference(Left + Tail, Right + Tail, static_cast<uint32>(Found));
                }
            }

            return 0;
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

    NODISCARD FORCEINLINE int32 Memcmp(const void* A, const void* B, size_t Size)
    {
        return Detail::CompareDispatch(A, B, Size);
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

    NODISCARD FORCEINLINE int32 Memcmp(const void* A, const void* B, size_t Size)
    {
        return std::memcmp(A, B, Size);
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
