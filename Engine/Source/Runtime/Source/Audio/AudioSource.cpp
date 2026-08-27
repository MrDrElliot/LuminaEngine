#include "RuntimePCH.h"
#include "AudioSource.h"

#include "Graph/AudioGraphInstance.h"
#include "Memory/Memory.h"
#include "ProceduralAudioStream.h"

namespace Lumina
{
	bool FWaveAudioSource::OpenReader(const void* Data, size_t Size)
	{
		Cursor = 0;
		return Reader.Open(Data, Size) && Reader.GetInfo().NumFrames != 0;
	}

	bool FWaveAudioSource::Open(const TSharedPtr<FAudioData>& Data)
	{
		if (!Data || Data->Bytes.empty())
		{
			return false;
		}

		Shared = Data;
		return OpenReader(Shared->Bytes.data(), Shared->Bytes.size());
	}

	bool FWaveAudioSource::Open(TVector<uint8>&& Bytes)
	{
		Owned = Move(Bytes);
		if (Owned.empty())
		{
			return false;
		}

		return OpenReader(Owned.data(), Owned.size());
	}

	bool FWaveAudioSource::IsAtEnd() const
	{
		return !bLooping && Cursor >= Reader.GetInfo().NumFrames;
	}

	void FWaveAudioSource::Seek(uint64 Frame)
	{
		const uint64 Total = Reader.GetInfo().NumFrames;
		Cursor = Total != 0 ? (Frame % Total) : 0;
	}

	uint32 FWaveAudioSource::Pull(float* Out, uint32 NumFrames)
	{
		if (!Reader.IsOpen() || Out == nullptr || NumFrames == 0)
		{
			return 0;
		}

		const uint64 Total    = Reader.GetInfo().NumFrames;
		const uint32 Channels = Reader.GetInfo().NumChannels;

		uint32 Written = 0;
		while (Written < NumFrames)
		{
			if (Cursor >= Total)
			{
				if (!bLooping)
				{
					break;
				}
				Cursor = 0;
			}

			const uint64 Read = Reader.ReadFrames(Cursor, NumFrames - Written, Out + (size_t)Written * Channels);
			if (Read == 0)
			{
				break;
			}

			Cursor  += Read;
			Written += (uint32)Read;
		}

		return Written;
	}

	FProceduralAudioSource::FProceduralAudioSource(const TSharedPtr<FProceduralAudioStream>& InStream)
		: Stream(InStream)
	{
	}

	uint32 FProceduralAudioSource::GetSampleRate() const
	{
		return Stream ? Stream->GetSampleRate() : 0;
	}

	uint32 FProceduralAudioSource::GetChannelCount() const
	{
		return Stream ? Stream->GetChannelCount() : 0;
	}

	uint32 FProceduralAudioSource::Pull(float* Out, uint32 NumFrames)
	{
		if (!Stream || Out == nullptr)
		{
			return 0;
		}

		// Reports the full request even on an underrun, because Read silence-pads and the voice lives on.
		Stream->Read(Out, NumFrames);
		return NumFrames;
	}

	FAudioGraphSource::FAudioGraphSource(const TSharedPtr<FAudioGraphInstance>& InInstance)
		: Instance(InInstance)
	{
	}

	uint32 FAudioGraphSource::GetSampleRate() const
	{
		return Instance ? Instance->GetSampleRate() : 0;
	}

	uint32 FAudioGraphSource::GetChannelCount() const
	{
		return Instance ? Instance->GetChannelCount() : 0;
	}

	bool FAudioGraphSource::IsAtEnd() const
	{
		return !Instance || Instance->IsFinished();
	}

	uint64 FAudioGraphSource::GetCursor() const
	{
		return Instance ? Instance->GetRenderedFrames() : 0;
	}

	uint32 FAudioGraphSource::Pull(float* Out, uint32 NumFrames)
	{
		if (!Instance || Out == nullptr)
		{
			return 0;
		}

		if (Instance->IsFinished())
		{
			Memory::Memzero(Out, (size_t)NumFrames * Instance->GetChannelCount() * sizeof(float));
			return 0;
		}

		Instance->Render(Out, NumFrames);
		return NumFrames;
	}
}
