#include "pch.h"
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

		if (GAudioContext == nullptr)
		{
			return;
		}

		if (!Stream)
		{
			Stream = GAudioContext->CreateProceduralStream(SampleRate, ChannelCount, BufferFrames);
			if (!Stream)
			{
				return;
			}
		}

		FAudioPlayParams Params;
		Params.Volume       = Volume;
		Params.Pitch        = Pitch;
		Params.bSpatialized = bSpatialized;
		Params.Bus          = Bus;
		Params.Attenuation  = Attenuation;

		ActiveHandle = GAudioContext->PlayProceduralStream(Stream, Params);

		bPlaying = ActiveHandle.IsValid();
	}

	void SProceduralAudioComponent::Stop()
	{
		if (GAudioContext != nullptr && bPlaying && ActiveHandle.IsValid())
		{
			GAudioContext->StopSound(ActiveHandle, EAudioStopMode::Immediate);
			ActiveHandle = FAudioHandle::Invalid();
			bPlaying = false;
		}
	}

	uint32 SProceduralAudioComponent::QueueSamples(const TVector<float>& Samples)
	{
		if (!Stream || ChannelCount == 0)
		{
			return 0;
		}

		const uint32 NumFrames = (uint32)(Samples.size() / ChannelCount);
		if (NumFrames == 0)
		{
			return 0;
		}

		return Stream->Write(Samples.data(), NumFrames);
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
