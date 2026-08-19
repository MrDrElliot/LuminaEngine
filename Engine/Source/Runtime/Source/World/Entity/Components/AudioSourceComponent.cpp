#include "RuntimePCH.h"
#include "AudioSourceComponent.h"

#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/AudioGlobals.h"

namespace Lumina
{
	void SAudioSourceComponent::Play()
	{
		if (!Audio::HasDevice())
		{
			return;
		}

		if (bPlaying && ActiveHandle.IsValid())
		{
			Audio::Context().StopSound(ActiveHandle);
		}

		if (Sound == nullptr || !Sound->IsValid())
		{
			return;
		}

		FAudioPlayParams Params;
		Params.Volume            = Volume;
		Params.Pitch             = Pitch;
		Params.bLooping          = bLooping;
		Params.bSpatialized      = bSpatialized;
		Params.Position          = LastPosition;
		Params.Bus               = Bus;
		Params.Attenuation       = Attenuation.Resolve();
		Params.Priority          = (uint8)Math::Clamp(Priority, 0, 255);
		Params.FadeInSeconds     = FadeInTime;
		Params.bUseOcclusion     = Occlusion.bEnabled;

		ActiveHandle = Audio::Context().PlayAudio(Sound->GetAudioData(), Params);

		bPlaying = ActiveHandle.IsValid();
		bPaused  = false;

		OcclusionTarget  = 0.0f;
		OcclusionCurrent = 0.0f;
		OcclusionTraceTimer = 0.0f;
	}

	void SAudioSourceComponent::Stop()
	{
		StopWithMode(EAudioStopMode::Immediate);
	}

	void SAudioSourceComponent::FadeOut()
	{
		StopWithMode(EAudioStopMode::AllowFadeOut);
	}

	void SAudioSourceComponent::StopWithMode(EAudioStopMode Mode)
	{
		if (Audio::HasDevice() && bPlaying && ActiveHandle.IsValid())
		{
			Audio::Context().StopSound(ActiveHandle, Mode, FadeOutTime);
		}

		ActiveHandle = FAudioHandle::Invalid();
		bPlaying = false;
		bPaused  = false;
	}

	void SAudioSourceComponent::SetPaused(bool bInPaused)
	{
		if (!Audio::HasDevice() || !bPlaying || !ActiveHandle.IsValid())
		{
			return;
		}

		Audio::Context().SetPaused(ActiveHandle, bInPaused);
		bPaused = bInPaused;
	}

	bool SAudioSourceComponent::IsPlaying() const
	{
		// The no-op context reports every handle as Free, so this answers false with no device.
		return Audio::Context().GetVoiceState(ActiveHandle) != EAudioVoiceState::Free;
	}

	float SAudioSourceComponent::GetPlaybackTime() const
	{
		if (!Audio::HasDevice() || Sound == nullptr || !Sound->IsValid())
		{
			return 0.0f;
		}

		const uint32 SampleRate = Sound->SampleRate;
		if (SampleRate == 0)
		{
			return 0.0f;
		}

		return (float)((double)Audio::Context().GetPlaybackFrame(ActiveHandle) / (double)SampleRate);
	}

	void SAudioSourceComponent::SeekToTime(float Seconds)
	{
		if (!Audio::HasDevice() || Sound == nullptr || !Sound->IsValid())
		{
			return;
		}

		Audio::Context().SeekToFrame(ActiveHandle, (uint64)(Math::Max(Seconds, 0.0f) * Sound->SampleRate));
	}
}
