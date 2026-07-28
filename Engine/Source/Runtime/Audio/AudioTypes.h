#pragma once

#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "AudioTypes.generated.h"

namespace Lumina
{
	// Immutable encoded audio bytes (e.g. a .wav file image) shared between an asset and any
	// in-flight sounds; the audio pump decodes from these bytes, so they must stay alive for
	// the duration of playback. Share via TSharedPtr.
	struct FAudioData
	{
		TVector<uint8> Bytes;
	};

	/** Mix group a voice routes through. Every bus feeds Master, so Master's volume scales everything. */
	REFLECT()
	enum class RUNTIME_API EAudioBus : uint8
	{
		Master,
		Music,
		SFX,
		UI,
		Voice,
		Ambient,
	};

	inline constexpr uint32 NumAudioBuses = 6;

	RUNTIME_API const char* ToString(EAudioBus Bus);

	/** Distance falloff curve applied to a spatialized voice between MinDistance and MaxDistance. */
	REFLECT()
	enum class RUNTIME_API EAudioAttenuationModel : uint8
	{
		/** No distance falloff; the voice plays at full gain anywhere in the world. */
		None,

		/** 1/d falloff. Physically correct, the default for point sources. */
		Inverse,

		/** Straight ramp from full gain to silence. Predictable, good for gameplay-critical cues. */
		Linear,

		/** Steep power curve; falls off faster than inverse near the source. */
		Exponential,
	};

	/** Whether a voice's position is world space or relative to the listener. */
	REFLECT()
	enum class RUNTIME_API EAudioPositioning : uint8
	{
		Absolute,
		Relative,
	};

	/** 3D falloff, cone and doppler behavior for a single voice. */
	REFLECT()
	struct RUNTIME_API SAudioAttenuation
	{
		GENERATED_BODY()

		PROPERTY(Script, Editable, Category = "Attenuation")
		EAudioAttenuationModel Model = EAudioAttenuationModel::Inverse;

		/** Distance (meters) the voice stays at full gain within. */
		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = 0.0f)
		float MinDistance = 1.0f;

		/** Distance (meters) the falloff curve reaches its minimum gain at. */
		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = 0.0f)
		float MaxDistance = 50.0f;

		/** Falloff steepness past MinDistance. Higher = quieter sooner. */
		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = 0.0f)
		float Rolloff = 1.0f;

		/** Gain floor; keeps distant voices audible when > 0. */
		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = 0.0f, ClampMax = 1.0f)
		float MinGain = 0.0f;

		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = 0.0f, ClampMax = 1.0f)
		float MaxGain = 1.0f;

		/** Degrees. The voice plays at full gain inside this cone around its forward axis. */
		PROPERTY(Script, Editable, Category = "Cone", ClampMin = 0.0f, ClampMax = 360.0f)
		float ConeInnerAngle = 360.0f;

		/** Degrees. Gain ramps from full to ConeOuterGain between the inner and outer angle. */
		PROPERTY(Script, Editable, Category = "Cone", ClampMin = 0.0f, ClampMax = 360.0f)
		float ConeOuterAngle = 360.0f;

		PROPERTY(Script, Editable, Category = "Cone", ClampMin = 0.0f, ClampMax = 1.0f)
		float ConeOuterGain = 0.0f;

		/** Pitch shift from relative listener/emitter motion. 0 disables doppler. */
		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = 0.0f, ClampMax = 10.0f)
		float DopplerFactor = 1.0f;

		/** How much the listener's facing attenuates the voice. 0 = ignore facing. */
		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = 0.0f, ClampMax = 1.0f)
		float DirectionalFactor = 0.0f;

		/** Stereo pan applied after spatialization. -1 = hard left, +1 = hard right. */
		PROPERTY(Script, Editable, Category = "Attenuation", ClampMin = -1.0f, ClampMax = 1.0f)
		float Pan = 0.0f;

		PROPERTY(Script, Editable, Category = "Attenuation")
		EAudioPositioning Positioning = EAudioPositioning::Absolute;
	};

	/** Raycast-driven muffling applied to a spatialized voice when geometry blocks the listener. */
	REFLECT()
	struct RUNTIME_API SAudioOcclusion
	{
		GENERATED_BODY()

		/** Trace from the listener to this voice every OcclusionInterval and muffle it when blocked. */
		PROPERTY(Script, Editable, Category = "Occlusion")
		bool bEnabled = false;

		/** Low-pass cutoff (Hz) at full occlusion. Lower = more muffled. */
		PROPERTY(Script, Editable, Category = "Occlusion", ClampMin = 80.0f, ClampMax = 22000.0f)
		float LowPassFrequency = 700.0f;

		/** Gain multiplier at full occlusion. */
		PROPERTY(Script, Editable, Category = "Occlusion", ClampMin = 0.0f, ClampMax = 1.0f)
		float VolumeAttenuation = 0.5f;

		/** Seconds to blend fully between occluded and unoccluded. Keeps the filter from popping. */
		PROPERTY(Script, Editable, Category = "Occlusion", ClampMin = 0.0f)
		float InterpTime = 0.35f;

		/** Seconds between occlusion traces. Traces are staggered across voices. */
		PROPERTY(Script, Editable, Category = "Occlusion", ClampMin = 0.0f)
		float TraceInterval = 0.2f;
	};

	/** Identifies one playing voice. Index is the context's voice slot, Generation disambiguates reuse. */
	struct FAudioHandle
	{
		uint32 Generation = 0;
		uint32 Index      = 0;

		constexpr bool IsValid() const { return Generation != 0; }

		constexpr bool operator==(const FAudioHandle& Other) const
		{
			return Generation == Other.Generation && Index == Other.Index;
		}

		constexpr bool operator!=(const FAudioHandle& Other) const
		{
			return !(*this == Other);
		}

		static constexpr FAudioHandle Invalid() { return FAudioHandle{}; }
	};

	/** Published per voice slot so the game thread can query a voice without touching the mixer. */
	enum class EAudioVoiceState : uint8
	{
		Free,
		Playing,
		Paused,
	};

	/** Everything needed to start a voice. Passed by value into the pump's pending-play queue. */
	struct FAudioPlayParams
	{
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool bLooping = false;
		bool bSpatialized = false;
		bool bStartPaused = false;

		FVector3 Position = FVector3(0.0f);
		FVector3 Velocity = FVector3(0.0f);
		FVector3 Direction = FVector3(0.0f, 0.0f, 1.0f);

		EAudioBus Bus = EAudioBus::SFX;
		SAudioAttenuation Attenuation;

		/** Voices are evicted lowest-priority-first when every slot is taken. */
		uint8 Priority = 128;

		/** PCM frame to begin playback at. */
		uint64 StartFrame = 0;

		float FadeInSeconds = 0.0f;

		/** Delays the voice without occupying the mixer, e.g. for scheduled one-shots. */
		float StartDelaySeconds = 0.0f;

		/** Reserve an occlusion filter up front so the first trace result applies without a pop. */
		bool bUseOcclusion = false;
	};

	enum class EAudioCommandType : uint8
	{
		StopSound,
		StopAll,
		SetVolume,
		SetPitch,
		SetLooping,
		SetPosition,
		SetVelocity,
		SetDirection,
		SetAttenuation,
		SetMinMaxDistance,
		SetPan,
		SetPaused,
		SetOcclusion,
		SetLowPassCutoff,
		FadeTo,
		SeekToFrame,
		SetBus,
		SetPriority,
		UpdateListener,
	};

	enum class EAudioStopMode : uint8
	{
		Immediate,
		AllowFadeOut,
	};
}
