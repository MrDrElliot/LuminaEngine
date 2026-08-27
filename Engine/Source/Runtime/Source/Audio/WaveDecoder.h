#pragma once

#include "AudioDecode.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Audio
{
	/** Sample encoding declared by a WAVE fmt chunk. */
	enum class EWaveSampleFormat : uint8
	{
		Unknown,
		PCM8,
		PCM16,
		PCM24,
		PCM32,
		Float32,
		Float64,
		ALaw,
		MuLaw,
	};

	RUNTIME_API const char* ToString(EWaveSampleFormat Format);

	/** Random-access RIFF/WAVE reader over a file image the caller keeps alive for the reader's lifetime. */
	class RUNTIME_API FWaveReader
	{
	public:

		// Parses the header and locates the data chunk without decoding any samples.
		bool Open(const void* Data, size_t Size);

		bool IsOpen() const { return SampleFormat != EWaveSampleFormat::Unknown; }

		const FAudioInfo& GetInfo() const { return Info; }
		EWaveSampleFormat GetSampleFormat() const { return SampleFormat; }

		// Decodes interleaved float32 at the source rate, returning frames written which falls short at end of data.
		uint64 ReadFrames(uint64 FrameOffset, uint64 NumFrames, float* Out) const;

	private:

		FAudioInfo Info;
		EWaveSampleFormat SampleFormat = EWaveSampleFormat::Unknown;

		// Points into the caller's buffer at the first byte of the data chunk.
		const uint8* Samples = nullptr;

		uint32 BytesPerFrame = 0;
	};
}
