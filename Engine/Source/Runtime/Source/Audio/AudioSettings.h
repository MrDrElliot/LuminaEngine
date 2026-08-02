#pragma once

#include "AudioTypes.h"
#include "Config/DeveloperSettings.h"
#include "Core/Object/ObjectMacros.h"
#include "Physics/PhysicsTypes.h"
#include "AudioSettings.generated.h"

namespace Lumina
{
	// Project-wide audio configuration. Bus volumes are the shipped defaults; a game's options menu
	// writes them back through Audio::SetBusVolume + FConfig::SaveSettings.
	REFLECT(MinimalAPI, ConfigFile = "/Config/AudioSettings.json", DisplayName = "Audio", Category = "Engine")
	class CAudioSettings : public CDeveloperSettings
	{
		GENERATED_BODY()
	public:

		/** Scales every bus. */
		PROPERTY(Editable, Category = "Mix", ClampMin = 0.0f, ClampMax = 1.0f)
		float MasterVolume = 1.0f;

		PROPERTY(Editable, Category = "Mix", ClampMin = 0.0f, ClampMax = 1.0f)
		float MusicVolume = 1.0f;

		PROPERTY(Editable, Category = "Mix", ClampMin = 0.0f, ClampMax = 1.0f)
		float SFXVolume = 1.0f;

		PROPERTY(Editable, Category = "Mix", ClampMin = 0.0f, ClampMax = 1.0f)
		float UIVolume = 1.0f;

		PROPERTY(Editable, Category = "Mix", ClampMin = 0.0f, ClampMax = 1.0f)
		float VoiceVolume = 1.0f;

		PROPERTY(Editable, Category = "Mix", ClampMin = 0.0f, ClampMax = 1.0f)
		float AmbientVolume = 1.0f;

		/** Suspends the output device while the app is in the background. */
		PROPERTY(Editable, Category = "Mix")
		bool bMuteWhenUnfocused = true;

		/** Mixer sample rate. 0 uses the output device's native rate. Changing this rebuilds the device. */
		PROPERTY(Editable, Category = "Output")
		uint32 SampleRate = 48000;

		/** Mixer channel count. 0 uses the output device's native layout. */
		PROPERTY(Editable, Category = "Output")
		uint32 Channels = 0;

		/** Mixer period in frames. 0 lets the backend choose. Smaller = lower latency, more CPU. */
		PROPERTY(Editable, Category = "Output")
		uint32 PeriodFrames = 0;

		/** Hard cap on simultaneous voices. New voices evict lower-priority ones once this is hit. */
		PROPERTY(Editable, Category = "Output", ClampMin = 8, ClampMax = 256)
		uint32 MaxVoices = 128;

		/** Milliseconds of ramp applied to volume changes. Kills clicks on abrupt gain edits. */
		PROPERTY(Editable, Category = "Output", ClampMin = 0.0f, ClampMax = 200.0f)
		float VolumeSmoothingMs = 10.0f;

		/** Global doppler multiplier. 0 disables doppler engine-wide. */
		PROPERTY(Editable, Category = "Spatialization", ClampMin = 0.0f, ClampMax = 10.0f)
		float DopplerScale = 1.0f;

		/** Master switch. When off, audio sources skip occlusion tracing entirely. */
		PROPERTY(Editable, Category = "Occlusion")
		bool bOcclusionEnabled = true;

		/** Collision layers an occlusion trace treats as blocking. */
		PROPERTY(Editable, Category = "Occlusion")
		ECollisionProfiles OcclusionTraceChannel = ECollisionProfiles::Static;

		/** Trace budget per world tick. Sources past the budget keep their last result. */
		PROPERTY(Editable, Category = "Occlusion", ClampMin = 1, ClampMax = 256)
		uint32 MaxOcclusionTracesPerTick = 16;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float ReverbRoomSize = 0.5f;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float ReverbDamping = 0.5f;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float ReverbWidth = 1.0f;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 2.0f)
		float ReverbWetLevel = 0.35f;

		/** Per-bus send into the reverb return. 0 leaves the bus dry and costs nothing. */
		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float MusicReverbSend = 0.0f;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float SFXReverbSend = 0.0f;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float UIReverbSend = 0.0f;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float VoiceReverbSend = 0.0f;

		PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
		float AmbientReverbSend = 0.0f;

		float GetBusVolume(EAudioBus Bus) const;
		void SetBusVolume(EAudioBus Bus, float Volume);
		float GetBusReverbSend(EAudioBus Bus) const;

		void PostInitSettings() override;
	};
}
