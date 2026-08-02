#include "RuntimePCH.h"
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
		GAudioContext = new FMiniaudioContext{};
	}

	void Audio::Shutdown()
	{
		delete GAudioContext;
		GAudioContext = nullptr;
	}

	void Audio::Update()
	{
		if (GAudioContext == nullptr)
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

			GAudioContext->SetSuspended(bShouldSuspend);
		}

		GAudioContext->Update();
	}

	void Audio::ApplySettings()
	{
		if (GAudioContext == nullptr)
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
			GAudioContext->SetBusVolume(Bus, Settings->GetBusVolume(Bus));
			GAudioContext->SetBusReverbSend(Bus, Settings->GetBusReverbSend(Bus));
		}

		GAudioContext->SetDopplerScale(Settings->DopplerScale);
		GAudioContext->SetMaxVoiceCount(Settings->MaxVoices);
		GAudioContext->SetVolumeSmoothing(Settings->VolumeSmoothingMs);

		FAudioReverbParams Reverb;
		Reverb.RoomSize = Settings->ReverbRoomSize;
		Reverb.Damping  = Settings->ReverbDamping;
		Reverb.Width    = Settings->ReverbWidth;
		Reverb.WetLevel = Settings->ReverbWetLevel;
		GAudioContext->SetReverbParams(Reverb);

		GAudioContext->ApplyDeviceConfig(Settings->SampleRate, Settings->Channels, Settings->PeriodFrames);
	}
}
