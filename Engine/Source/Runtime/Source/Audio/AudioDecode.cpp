#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AudioDecode.h"

#include "WaveDecoder.h"

namespace Lumina::Audio
{
	bool Probe(const void* Data, size_t Size, FAudioInfo& OutInfo)
	{
		FWaveReader Reader;
		if (!Reader.Open(Data, Size))
		{
			return false;
		}

		OutInfo = Reader.GetInfo();
		return true;
	}

	bool DecodePCM(const void* Data, size_t Size, FAudioInfo& OutInfo, TVector<float>& OutSamples)
	{
		LUMINA_MEMORY_SCOPE("Audio");

		FWaveReader Reader;
		if (!Reader.Open(Data, Size))
		{
			return false;
		}

		const FAudioInfo Info = Reader.GetInfo();

		OutSamples.clear();
		OutSamples.resize((size_t)(Info.NumFrames * Info.NumChannels));

		const uint64 FramesRead = Reader.ReadFrames(0, Info.NumFrames, OutSamples.data());
		OutSamples.resize((size_t)(FramesRead * Info.NumChannels));

		OutInfo = Info;
		OutInfo.NumFrames = FramesRead;
		return FramesRead != 0;
	}
}
