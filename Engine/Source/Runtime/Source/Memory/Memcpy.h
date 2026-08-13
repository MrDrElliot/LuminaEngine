#pragma once

#include <immintrin.h>

namespace Lumina::Memory
{
	FORCEINLINE void Memcpy(void* RESTRICT Destination, void* RESTRICT Source, size_t SrcSize)
	{
		std::memcpy(Destination, Source, SrcSize);
	}

	FORCEINLINE void Memcpy(void* RESTRICT Destination, const void* RESTRICT Source, size_t SrcSize)
	{
		std::memcpy(Destination, Source, SrcSize);
	}
	
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