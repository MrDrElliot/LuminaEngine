#include "RuntimePCH.h"
#include "AudioSourceComponent.h"

#include "Assets/AssetTypes/Audio/AudioGraph.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/AudioGlobals.h"
#include "Audio/Graph/AudioGraphInstance.h"
#include "Audio/SoundPlayback.h"
#include "Core/Object/Cast.h"

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

		GraphInstance.reset();

		if (Sound == nullptr || !Sound->IsPlayable())
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

		const FSoundPlayResult Started = Audio::PlaySound(Sound.Get(), Params);
		ActiveHandle  = Started.Handle;
		GraphInstance = Started.GraphInstance;

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
		GraphInstance.reset();
		TriggerOutputCursors.clear();
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

	bool SAudioSourceComponent::IsPersistent() const
	{
		// A graph decides its own lifetime, so bLooping never speaks for one.
		if (const CAudioGraph* Graph = Cast<CAudioGraph>(Sound.Get()))
		{
			return Graph->IsEndless();
		}

		return bLooping;
	}

	float SAudioSourceComponent::GetPlaybackTime() const
	{
		if (!Audio::HasDevice())
		{
			return 0.0f;
		}

		if (GraphInstance)
		{
			return (float)((double)GraphInstance->GetRenderedFrames() / (double)GraphInstance->GetSampleRate());
		}

		const CAudioStream* Stream = Cast<CAudioStream>(Sound.Get());
		if (Stream == nullptr || Stream->SampleRate == 0)
		{
			return 0.0f;
		}

		return (float)((double)Audio::Context().GetPlaybackFrame(ActiveHandle) / (double)Stream->SampleRate);
	}

	void SAudioSourceComponent::SeekToTime(float Seconds)
	{
		if (!Audio::HasDevice())
		{
			return;
		}

		// A graph has no timeline to seek along, only a rewind, which Play already covers.
		const CAudioStream* Stream = Cast<CAudioStream>(Sound.Get());
		if (Stream == nullptr || !Stream->IsValid())
		{
			return;
		}

		Audio::Context().SeekToFrame(ActiveHandle, (uint64)(Math::Max(Seconds, 0.0f) * Stream->SampleRate));
	}

	void SAudioSourceComponent::SetFloatParameter(FName Name, float Value)
	{
		if (GraphInstance)
		{
			GraphInstance->SetFloatParameter(Name, Value);
		}
	}

	void SAudioSourceComponent::SetIntParameter(FName Name, int32 Value)
	{
		if (GraphInstance)
		{
			GraphInstance->SetIntParameter(Name, Value);
		}
	}

	void SAudioSourceComponent::SetBoolParameter(FName Name, bool Value)
	{
		if (GraphInstance)
		{
			GraphInstance->SetBoolParameter(Name, Value);
		}
	}

	void SAudioSourceComponent::TriggerParameter(FName Name)
	{
		if (GraphInstance)
		{
			GraphInstance->TriggerParameter(Name);
		}
	}

	float SAudioSourceComponent::GetFloatOutput(FName Name) const
	{
		return GraphInstance ? GraphInstance->GetFloatOutput(Name) : 0.0f;
	}

	int32 SAudioSourceComponent::ConsumeTriggerOutput(FName Name)
	{
		if (!GraphInstance)
		{
			return 0;
		}

		const uint32 Total = GraphInstance->GetTriggerOutputCount(Name);

		for (TPair<FName, uint32>& Seen : TriggerOutputCursors)
		{
			if (Seen.first == Name)
			{
				const uint32 Fired = Total - Seen.second;
				Seen.second = Total;
				return (int32)Fired;
			}
		}

		// First ask reports nothing, so a script that starts watching midway does not get a backlog.
		TriggerOutputCursors.push_back(TPair<FName, uint32>(Name, Total));
		return 0;
	}
}
