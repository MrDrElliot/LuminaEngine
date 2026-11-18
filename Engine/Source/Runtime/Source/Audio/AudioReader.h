#pragma once

#include "QoaCodec.h"
#include "WaveDecoder.h"

namespace Lumina::Audio
{
	// Every encoding an audio asset can hold behind one random-access interface, chosen from the file's magic.
	class RUNTIME_API FAudioReader
	{
	public:

		bool Open(const void* Data, size_t Size)
		{
			bQoa = Qoa::IsQoa(Data, Size);
			return bQoa ? QoaReader.Open(Data, Size) : WaveReader.Open(Data, Size);
		}

		bool IsOpen() const { return bQoa ? QoaReader.IsOpen() : WaveReader.IsOpen(); }

		const FAudioInfo& GetInfo() const { return bQoa ? QoaReader.GetInfo() : WaveReader.GetInfo(); }

		uint64 ReadFrames(uint64 FrameOffset, uint64 NumFrames, float* Out) const
		{
			return bQoa ? QoaReader.ReadFrames(FrameOffset, NumFrames, Out) : WaveReader.ReadFrames(FrameOffset, NumFrames, Out);
		}

	private:

		FWaveReader WaveReader;
		FQoaReader  QoaReader;
		bool        bQoa = false;
	};
}
