#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AudioDecode.h"

#include "AudioReader.h"

namespace Lumina::Audio
{
	bool Probe(const void* Data, size_t Size, FAudioInfo& OutInfo)
	{
		FAudioReader Reader;
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

		FAudioReader Reader;
		if (!Reader.Open(Data, Size))
		{
			return false;
		}

		const FAudioInfo Info = Reader.GetInfo();

		OutSamples.Reset();
		OutSamples.SetNum(IntCastChecked<int32>(Info.NumFrames * Info.NumChannels));

		const uint64 FramesRead = Reader.ReadFrames(0, Info.NumFrames, OutSamples.GetData());
		OutSamples.SetNum(IntCastChecked<int32>(FramesRead * Info.NumChannels));

		OutInfo = Info;
		OutInfo.NumFrames = FramesRead;
		return FramesRead != 0;
	}

	bool IsCompressed(const void* Data, size_t Size)
	{
		return Qoa::IsQoa(Data, Size);
	}

	bool Compress(const void* Data, size_t Size, TVector<uint8>& Out)
	{
		FAudioInfo Info;
		TVector<float> Samples;
		return DecodePCM(Data, Size, Info, Samples) && Qoa::Encode(Samples.GetData(), Info.NumFrames, Info.NumChannels, Info.SampleRate, Out);
	}
}
