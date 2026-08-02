#pragma once

#include "Containers/Array.h"
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

	// Reads format metadata from an encoded audio file image (wav/mp3/flac/ogg) without decoding samples.
	RUNTIME_API bool Probe(const void* Data, size_t Size, FAudioInfo& OutInfo);

	// Decodes the full file into interleaved float32 PCM at the source rate/channel count.
	RUNTIME_API bool DecodePCM(const void* Data, size_t Size, FAudioInfo& OutInfo, TVector<float>& OutSamples);
}
