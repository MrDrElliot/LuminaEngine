#include "pch.h"
#include "AudioSettings.h"

#include "AudioContext.h"

namespace Lumina
{
	float CAudioSettings::GetBusVolume(EAudioBus Bus) const
	{
		switch (Bus)
		{
		case EAudioBus::Master:  return MasterVolume;
		case EAudioBus::Music:   return MusicVolume;
		case EAudioBus::SFX:     return SFXVolume;
		case EAudioBus::UI:      return UIVolume;
		case EAudioBus::Voice:   return VoiceVolume;
		case EAudioBus::Ambient: return AmbientVolume;
		}
		return 1.0f;
	}

	void CAudioSettings::SetBusVolume(EAudioBus Bus, float Volume)
	{
		switch (Bus)
		{
		case EAudioBus::Master:  MasterVolume = Volume;  break;
		case EAudioBus::Music:   MusicVolume = Volume;   break;
		case EAudioBus::SFX:     SFXVolume = Volume;     break;
		case EAudioBus::UI:      UIVolume = Volume;      break;
		case EAudioBus::Voice:   VoiceVolume = Volume;   break;
		case EAudioBus::Ambient: AmbientVolume = Volume; break;
		}
	}

	float CAudioSettings::GetBusReverbSend(EAudioBus Bus) const
	{
		switch (Bus)
		{
		case EAudioBus::Master:  return 0.0f;
		case EAudioBus::Music:   return MusicReverbSend;
		case EAudioBus::SFX:     return SFXReverbSend;
		case EAudioBus::UI:      return UIReverbSend;
		case EAudioBus::Voice:   return VoiceReverbSend;
		case EAudioBus::Ambient: return AmbientReverbSend;
		}
		return 0.0f;
	}

	void CAudioSettings::PostInitSettings()
	{
		Super::PostInitSettings();
		Audio::ApplySettings();
	}
}
