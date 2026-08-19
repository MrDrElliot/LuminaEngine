#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AudioContext.h"

#include "AudioGlobals.h"
#include "AudioSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Windows/Window.h"
#include "Miniaudio/MiniaudioContext.h"


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
		Audio::Internal::SetContext(new FMiniaudioContext{});
	}

	void Audio::Shutdown()
	{
		// Unpublish before destroying: anything still calling audio during teardown lands on the
		// no-op context rather than a dangling device.
		delete Audio::Internal::SetContext(nullptr);
	}

	void Audio::Update()
	{
		// A real HasDevice case: with no device there is no suspend state to track and no pump to run,
		// so this is skipped work rather than a guard against Context().
		if (!HasDevice())
		{
			return;
		}

		if (FWindow* Window = Windowing::GetPrimaryWindowHandle())
		{
			bool bShouldSuspend = Window->IsWindowMinimized();

			#if !USING(WITH_EDITOR)
			// Editor builds skip the focus test: PIE preview windows are separate native windows, so an
			// unfocused primary window doesn't mean the user has left the app.
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
