#pragma once

#include "Audio/AudioTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AudioSourceComponent.generated.h"

namespace Lumina
{
	class CAudioStream;

	REFLECT(Component, Category = "Audio")
	struct RUNTIME_API SAudioSourceComponent
	{
		GENERATED_BODY()

		/** Audio asset to play. */
		PROPERTY(Editable)
		TObjectPtr<CAudioStream> Sound;

		/** Mix group this source routes through. */
		PROPERTY(Editable)
		EAudioBus Bus = EAudioBus::SFX;

		/** Playback volume multiplier (1.0 = full volume). */
		PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 4.0f)
		float Volume = 1.0f;

		/** Playback pitch multiplier (1.0 = original pitch). */
		PROPERTY(Editable, ClampMin = 0.01f, ClampMax = 4.0f)
		float Pitch = 1.0f;

		/** When false the source plays as a flat 2D sound, ignoring its transform. */
		PROPERTY(Editable)
		bool bSpatialized = true;

		/** Distance falloff, cone and doppler behavior. */
		PROPERTY(Editable)
		SAudioAttenuation Attenuation;

		/** Muffling applied when level geometry blocks the line to the listener. */
		PROPERTY(Editable)
		SAudioOcclusion Occlusion;

		/** Voices with a lower priority are evicted first when the voice cap is reached. */
		PROPERTY(Editable, ClampMin = 0, ClampMax = 255)
		int32 Priority = 128;

		/** Seconds to ramp up from silence when playback starts. */
		PROPERTY(Editable, ClampMin = 0.0f)
		float FadeInTime = 0.0f;

		/** Seconds to ramp down to silence when Stop() is asked to fade. */
		PROPERTY(Editable, ClampMin = 0.0f)
		float FadeOutTime = 0.5f;

		/** When true, the sound restarts automatically upon completion. */
		PROPERTY(Editable)
		bool bLooping = false;

		/** When true, playback starts automatically once the component is initialized. */
		PROPERTY(Editable)
		bool bPlayOnReady = false;

		/** Skips starting the voice when the listener is beyond MaxDistance, freeing it for audible sounds. */
		PROPERTY(Editable)
		bool bCullBeyondMaxDistance = true;

		// Handle to the currently playing sound instance.
		FAudioHandle ActiveHandle;

		// Set once the component has been initialized by the audio system.
		bool bReady = false;

		// True when the sound is currently playing (or paused).
		bool bPlaying = false;
		bool bPaused = false;

		// Dirty flags for parameter changes.
		bool bVolumeDirty  = false;
		bool bPitchDirty   = false;
		bool bLoopingDirty = false;
		bool bAttenuationDirty = false;

		// Occlusion state owned by the audio system: last trace result, the smoothed value pushed to
		// the mixer, and the countdown to the next trace.
		float OcclusionTarget = 0.0f;
		float OcclusionCurrent = 0.0f;
		float OcclusionTraceTimer = 0.0f;

		// World position sampled last tick; drives the emitter velocity used for doppler.
		FVector3 LastPosition = FVector3(0.0f);
		bool bHasLastPosition = false;

		FUNCTION()
		void Play();

		FUNCTION()
		void Stop();

		/** Stops over FadeOutTime rather than cutting the voice instantly. */
		FUNCTION()
		void FadeOut();

		FUNCTION()
		void SetPaused(bool bInPaused);

		/** True while the mixer still holds a voice for this source. */
		FUNCTION()
		bool IsPlaying() const;

		/** Current playback position in seconds, 0 when not playing. */
		FUNCTION()
		float GetPlaybackTime() const;

		FUNCTION()
		void SeekToTime(float Seconds);

		void StopWithMode(EAudioStopMode Mode);
	};

	REFLECT(Component, Category = "Audio")
	struct RUNTIME_API SAudioListenerComponent
	{
		GENERATED_BODY()

		/** Engine listener slot this component drives. Split screen uses one component per slot. */
		PROPERTY(Editable, ClampMin = 0, ClampMax = 3)
		int32 ListenerIndex = 0;

		/** Feeds the listener's velocity to the doppler calculation. */
		PROPERTY(Editable)
		bool bApplyDoppler = true;

		FVector3 LastPosition = FVector3(0.0f);
		bool bHasLastPosition = false;
	};
}
