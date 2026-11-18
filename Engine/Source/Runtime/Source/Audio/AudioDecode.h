#pragma once

#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Audio
{
	struct FAudioInfo
	{
		uint32 SampleRate  = 0;
		uint32 NumChannels = 0;
		uint64 NumFrames   = 0;

		double GetDuration() const { return SampleRate != 0 ? (double)NumFrames / (double)SampleRate : 0.0; }
	};

	// Reads format metadata from a RIFF/WAVE or QOA file image without decoding samples.
	RUNTIME_API bool Probe(const void* Data, size_t Size, FAudioInfo& OutInfo);

	// Decodes the full file into interleaved float32 PCM at the source rate/channel count.
	RUNTIME_API bool DecodePCM(const void* Data, size_t Size, FAudioInfo& OutInfo, TVector<float>& OutSamples);

	RUNTIME_API bool IsCompressed(const void* Data, size_t Size);

	// Re-encodes any decodable file image as QOA, about a fifth of 16 bit PCM and decoded a frame at a time during playback.
	RUNTIME_API bool Compress(const void* Data, size_t Size, TVector<uint8>& Out);
}
