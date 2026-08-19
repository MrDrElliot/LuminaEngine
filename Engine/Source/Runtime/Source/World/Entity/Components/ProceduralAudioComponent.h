#pragma once

#include "Assets/AssetTypes/Audio/SoundAttenuation.h"
#include "Audio/AudioTypes.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectMacros.h"
#include "Memory/SmartPtr.h"
#include "ProceduralAudioComponent.generated.h"

namespace Lumina
{
	class FProceduralAudioStream;

	// Streams PCM audio supplied at runtime, typically synthesized in script, through a lock-free
	// ring buffer to the audio thread. Use for engine sounds, synths, network audio, etc.
	REFLECT(Component, Category = "Audio")
	struct RUNTIME_API SProceduralAudioComponent
	{
		GENERATED_BODY()

		/** Sample rate of the supplied PCM data. Must be set before Start(). */
		PROPERTY(Editable)
		uint32 SampleRate = 48000;

		/** Channel count of the supplied PCM data (1 = mono, 2 = stereo). */
		PROPERTY(Editable)
		uint32 ChannelCount = 1;

		/** Capacity of the streaming ring buffer in frames. */
		PROPERTY(Editable)
		uint32 BufferFrames = 16384;

		PROPERTY(Editable)
		float Volume = 1.0f;

		PROPERTY(Editable)
		float Pitch = 1.0f;

		/** Mix group this stream routes through. */
		PROPERTY(Editable)
		EAudioBus Bus = EAudioBus::SFX;

		/** Distance falloff, cone and doppler behavior. */
		PROPERTY(Editable)
		SAudioAttenuationSettings Attenuation;

		/** When true, the sound is positioned in 3D space using the entity's transform. */
		PROPERTY(Editable)
		bool bSpatialized = true;

		/** When true, Start() is called automatically the first time the component is ticked. */
		PROPERTY(Editable)
		bool bPlayOnReady = false;

		FAudioHandle ActiveHandle;

		bool bReady = false;
		bool bPlaying = false;

		bool bVolumeDirty = false;
		bool bPitchDirty = false;

		// The stream is shared with the audio thread. Producer is the script/game thread.
		TSharedPtr<FProceduralAudioStream> Stream;

		// Begin streaming. Allocates the ring buffer on first call and starts an ma_sound on the
		// audio thread that pulls from it.
		FUNCTION()
		void Start();

		FUNCTION()
		void Stop();

		// Push interleaved float samples to the ring buffer. Returns the number of frames actually
		// written (may be less than requested if the buffer is full).
		FUNCTION()
		uint32 QueueSamples(const TVector<float>& Samples);

		// Frames currently sitting in the ring buffer waiting to be played.
		FUNCTION()
		uint32 GetQueuedFrameCount();

		// Frames of headroom available for QueueSamples.
		FUNCTION()
		uint32 GetFreeFrameCount();
	};
}
