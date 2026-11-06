#include "RuntimePCH.h"
#include "ProceduralAudioComponent.h"

#include "Audio/AudioGlobals.h"
#include "Audio/ProceduralAudioStream.h"

namespace Lumina
{
	void SProceduralAudioComponent::Start()
	{
		if (bPlaying)
		{
			return;
		}

		if (!Audio::HasDevice())
		{
			return;
		}

		if (!Stream)
		{
			Stream = Audio::Context().CreateProceduralStream(SampleRate, ChannelCount, BufferFrames);
			if (!Stream)
			{
				return;
			}
		}

		SAudioPlayParams Params;
		Params.Volume       = Volume;
		Params.Pitch        = Pitch;
		Params.bSpatialized = bSpatialized;
		Params.Bus          = Bus;
		Params.Attenuation  = Attenuation.Resolve();

		ActiveHandle = Audio::Context().PlayProceduralStream(Stream, Params);

		bPlaying = ActiveHandle.IsValid();
	}

	void SProceduralAudioComponent::Stop()
	{
		if (Audio::HasDevice() && bPlaying && ActiveHandle.IsValid())
		{
			Audio::Context().StopSound(ActiveHandle, EAudioStopMode::Immediate);
			ActiveHandle = SAudioHandle::Invalid();
			bPlaying = false;
		}
	}

	uint32 SProceduralAudioComponent::QueueSamples(const TVector<float>& Samples)
	{
		// The stream's own count, since the component's may have been edited after the stream was made.
		const uint32 StreamChannels = Stream ? Stream->GetChannelCount() : 0;
		if (StreamChannels == 0)
		{
			return 0;
		}

		const uint32 NumFrames = Samples.Num() / StreamChannels;
		if (NumFrames == 0)
		{
			return 0;
		}

		return Stream->Write(Samples.GetData(), NumFrames);
	}

	uint32 SProceduralAudioComponent::GetQueuedFrameCount()
	{
		return Stream ? Stream->GetAvailableReadFrames() : 0;
	}

	uint32 SProceduralAudioComponent::GetFreeFrameCount()
	{
		return Stream ? Stream->GetAvailableWriteFrames() : 0;
	}
}
