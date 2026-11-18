#pragma once

#include "AudioDecode.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Audio
{
	// The Quite OK Audio format, an LMS predictor with 3 bit residuals at 3.2 bits per sample, in frames that decode independently.
	namespace Qoa
	{
		inline constexpr uint32 kSliceLen       = 20;
		inline constexpr uint32 kSlicesPerFrame = 256;
		inline constexpr uint32 kFrameLen       = kSliceLen * kSlicesPerFrame;
		inline constexpr uint32 kMaxChannels    = 8;

		NODISCARD RUNTIME_API bool IsQoa(const void* Data, size_t Size);

		// Interleaved float PCM in [-1, 1] is quantized to 16 bits before it is encoded.
		RUNTIME_API bool Encode(const float* Interleaved, uint64 NumFrames, uint32 NumChannels, uint32 SampleRate, TVector<uint8>& Out);
	}

	// Random-access reader over a QOA file image the caller keeps alive, decoding a whole frame on first touch and serving reads from it.
	class RUNTIME_API FQoaReader
	{
	public:

		bool Open(const void* Data, size_t Size);

		bool IsOpen() const { return Bytes != nullptr; }

		const FAudioInfo& GetInfo() const { return Info; }

		// Decodes interleaved float32 at the source rate, returning frames written, which falls short at end of data.
		uint64 ReadFrames(uint64 FrameOffset, uint64 NumFrames, float* Out) const;

	private:

		bool DecodeFrame(uint64 FrameIndex) const;

		const uint8* Bytes = nullptr;
		size_t       Size  = 0;
		FAudioInfo   Info;

		// One voice owns one reader, so the cache needs no lock.
		mutable uint64         CachedFrame   = ~0ull;
		mutable uint32         CachedSamples = 0;
		mutable TVector<float> Cache;
	};
}
