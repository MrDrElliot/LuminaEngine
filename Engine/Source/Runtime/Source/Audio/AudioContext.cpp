#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AudioContext.h"

#include "AudioGlobals.h"
#include "AudioSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Log/Log.h"
#include "Core/Windows/Window.h"
#include "LuminaAudioContext.h"


namespace Lumina
{
	const char* ToString(EAudioBus Bus)
	{
		switch (Bus)
		{
		case EAudioBus::Master:  return "Master";
		case EAudioBus::Music:   return "Music";
		case EAudioBus::SFX:     return "SFX";
		case EAudioBus::UI:      return "UI";
		case EAudioBus::Voice:   return "Voice";
		case EAudioBus::Ambient: return "Ambient";
		}
		return "Unknown";
	}

	void Audio::Initialize()
	{
		LUMINA_MEMORY_SCOPE("Audio");
		Audio::Internal::SetContext(new FLuminaAudioContext{});
	}

	void Audio::Shutdown()
	{
		// Unpublished first, so teardown-time audio calls land on the no-op context, not a dangling device.
		delete Audio::Internal::SetContext(nullptr);
	}

	void Audio::Update()
	{
		// With no device there is no suspend state and no pump, so this is skipped work rather than a guard.
		if (!HasDevice())
		{
			return;
		}

		if (FWindow* Window = Windowing::TryGetPrimaryWindowHandle())
		{
			bool bShouldSuspend = Window->IsWindowMinimized();

			#if !USING(WITH_EDITOR)
			// PIE previews are separate native windows, so an unfocused primary window is not the app leaving.
			if (!bShouldSuspend)
			{
				const CAudioSettings* Settings = GetDefault<CAudioSettings>();
				if (Settings != nullptr && Settings->bMuteWhenUnfocused)
				{
					bShouldSuspend = !Windowing::IsNativeWindowFocused(Window->GetWindow());
				}
			}
			#endif

			Audio::Context().SetSuspended(bShouldSuspend);
		}

		Audio::Context().Update();
	}

	void Audio::ApplySettings()
	{
		if (!Audio::HasDevice())
		{
			return;
		}

		const CAudioSettings* Settings = GetDefault<CAudioSettings>();
		if (Settings == nullptr)
		{
			return;
		}

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			const EAudioBus Bus = (EAudioBus)i;
			Audio::Context().SetBusVolume(Bus, Settings->GetBusVolume(Bus));
			Audio::Context().SetBusReverbSend(Bus, Settings->GetBusReverbSend(Bus));
		}

		Audio::Context().SetDopplerScale(Settings->DopplerScale);
		Audio::Context().SetMaxVoiceCount(Settings->MaxVoices);
		Audio::Context().SetVolumeSmoothing(Settings->VolumeSmoothingMs);

		FAudioReverbParams Reverb;
		Reverb.RoomSize = Settings->ReverbRoomSize;
		Reverb.Damping  = Settings->ReverbDamping;
		Reverb.Width    = Settings->ReverbWidth;
		Reverb.WetLevel = Settings->ReverbWetLevel;
		Audio::Context().SetReverbParams(Reverb);

		Audio::Context().ApplyDeviceConfig(Settings->SampleRate, Settings->Channels, Settings->PeriodFrames);
	}
}
