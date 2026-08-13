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

	/** Copy INTO write-combined memory -- the mapped ReBAR staging aperture, which is where every upload
	 *  lands (EMemoryType::CPUWrite prefers host-visible DEVICE_LOCAL).
	 *
	 *  std::memcpy is the wrong tool there. MSVC lowers a large copy to `rep movsb`, which is tuned for
	 *  cached destinations and degrades badly against WC: measured ~0.5-1.2 GB/s staging a 4K BC7 mip,
	 *  i.e. 13-30 ms for 16 MiB on the game thread. Non-temporal stores are what WC memory wants -- they
	 *  fill the write-combining buffers a full 64-byte line at a time and never read the destination.
	 *
	 *  Use it ONLY for WC/uncached destinations; against ordinary cached memory plain memcpy wins, because
	 *  NT stores bypass the cache the reader is about to want. */
	FORCEINLINE void MemcpyToWriteCombined(void* RESTRICT Destination, const void* RESTRICT Source, size_t Size)
	{
		auto*       Dest = static_cast<unsigned char*>(Destination);
		const auto* Src  = static_cast<const unsigned char*>(Source);

		// Align to a 16-byte boundary first: the streaming stores below require it, and a misaligned run
		// would otherwise split every write-combining line for the whole copy.
		const size_t Misaligned = reinterpret_cast<uintptr_t>(Dest) & 15u;
		if (Misaligned != 0)
		{
			const size_t Head = (16u - Misaligned) < Size ? (16u - Misaligned) : Size;
			std::memcpy(Dest, Src, Head);
			Dest += Head;
			Src  += Head;
			Size -= Head;
		}

		// 64 bytes per iteration -- one full WC line, so each is combined and flushed once.
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

		// Streaming stores are weakly ordered; without this the copy is not necessarily visible to the
		// GPU by the time the command list referencing it is submitted.
		_mm_sfence();
	}
}