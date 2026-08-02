#pragma once

#include "AudioTypes.h"
#include "AudioCommand.h"
#include "AudioReverb.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"
#include "Platform/Platform.h"

namespace Lumina
{
    class FProceduralAudioStream;

	namespace Audio
	{
		void Initialize();
		void Shutdown();
		// Per-frame pump (drains queued commands + housekeeping). Call once per frame from the engine.
		void Update();

		// Push CAudioSettings onto the live context. Safe to call before a device exists.
		RUNTIME_API void ApplySettings();
	}

	struct FAudioDeviceInfo
	{
		uint32 SampleRate = 0;
		uint32 Channels = 0;
		uint32 PeriodFrames = 0;
		uint32 ListenerCount = 0;
	};

	// Thread-safe; commands queue and are drained by a per-frame pump job on the task pool.
	class RUNTIME_API IAudioContext
	{
	public:

		virtual ~IAudioContext() = default;

		// Drain queued audio commands + housekeeping (kicked once per frame). Default no-op.
		virtual void Update() {}

		NODISCARD virtual void* GetNative() const = 0;

		// Asset-backed playback: the shared bytes are decoded on the audio pump and kept alive for the
		// lifetime of the voice, so the caller's asset can be unloaded mid-playback.
		NODISCARD virtual FAudioHandle PlayAudio(const TSharedPtr<FAudioData>& Data, const FAudioPlayParams& Params) = 0;

		// Loose-file playback (VFS path). Reads on the pump, so a large file briefly occupies a worker.
		NODISCARD virtual FAudioHandle PlayFile(FStringView File, const FAudioPlayParams& Params) = 0;

		NODISCARD virtual FAudioHandle PlayProceduralStream(TSharedPtr<FProceduralAudioStream> Stream, const FAudioPlayParams& Params) = 0;

		virtual void StopSound(FAudioHandle Handle, EAudioStopMode Mode = EAudioStopMode::Immediate, float FadeSeconds = 0.5f) = 0;
		virtual void StopAllSounds(EAudioStopMode Mode = EAudioStopMode::Immediate, float FadeSeconds = 0.5f) = 0;

		virtual void SetVolume(FAudioHandle Handle, float Volume) = 0;
		virtual void SetPitch(FAudioHandle Handle, float Pitch) = 0;
		virtual void SetLooping(FAudioHandle Handle, bool bLooping) = 0;
		virtual void SetPosition(FAudioHandle Handle, FVector3 Position) = 0;
		virtual void SetVelocity(FAudioHandle Handle, FVector3 Velocity) = 0;
		virtual void SetDirection(FAudioHandle Handle, FVector3 Direction) = 0;
		virtual void SetAttenuation(FAudioHandle Handle, const SAudioAttenuation& Attenuation) = 0;
		virtual void SetMinMaxDistance(FAudioHandle Handle, float MinDistance, float MaxDistance) = 0;
		virtual void SetPan(FAudioHandle Handle, float Pan) = 0;
		virtual void SetPaused(FAudioHandle Handle, bool bPaused) = 0;
		virtual void SetBus(FAudioHandle Handle, EAudioBus Bus) = 0;
		virtual void SetPriority(FAudioHandle Handle, uint8 Priority) = 0;

		// Amount is 0 (clear line of sight) to 1 (fully blocked). Drives a per-voice low-pass plus a
		// gain multiplier; the caller is responsible for smoothing the value over time.
		virtual void SetOcclusion(FAudioHandle Handle, float Amount, float LowPassFrequency, float VolumeAttenuation) = 0;

		// Direct control of the per-voice low-pass, for effects that aren't occlusion (underwater, radio).
		// Pass 0 to bypass the filter.
		virtual void SetLowPassCutoff(FAudioHandle Handle, float CutoffHz) = 0;

		virtual void FadeTo(FAudioHandle Handle, float Volume, float Seconds) = 0;

		// Seeks a playing (non-procedural) sound to the given PCM frame.
		virtual void SeekToFrame(FAudioHandle Handle, uint64 Frame) = 0;

		NODISCARD virtual EAudioVoiceState GetVoiceState(FAudioHandle Handle) const = 0;
		NODISCARD virtual uint64 GetPlaybackFrame(FAudioHandle Handle) const = 0;
		NODISCARD virtual uint32 GetActiveVoiceCount() const = 0;
		NODISCARD virtual uint32 GetMaxVoiceCount() const = 0;
		NODISCARD virtual uint64 GetDroppedVoiceCount() const = 0;

		NODISCARD bool IsPlaying(FAudioHandle Handle) const { return GetVoiceState(Handle) == EAudioVoiceState::Playing; }

		virtual void UpdateListener(uint32 ListenerIndex, FVector3 Position, FQuat Rotation, FVector3 Velocity) = 0;
		virtual void SetListenerEnabled(uint32 ListenerIndex, bool bEnabled) = 0;
		NODISCARD virtual uint32 GetListenerCount() const = 0;

		virtual void SetBusVolume(EAudioBus Bus, float Volume) = 0;
		NODISCARD virtual float GetBusVolume(EAudioBus Bus) const = 0;
		virtual void SetBusMuted(EAudioBus Bus, bool bMuted) = 0;
		NODISCARD virtual bool IsBusMuted(EAudioBus Bus) const = 0;
		virtual void SetBusPitch(EAudioBus Bus, float Pitch) = 0;

		// 0 disables the bus's wet branch. The reverb network is built on first use.
		virtual void SetBusReverbSend(EAudioBus Bus, float SendLevel) = 0;
		NODISCARD virtual float GetBusReverbSend(EAudioBus Bus) const = 0;

		virtual void SetReverbParams(const FAudioReverbParams& Params) = 0;
		NODISCARD virtual FAudioReverbParams GetReverbParams() const = 0;

		// Global multiplier on every voice's doppler factor. 0 disables doppler engine-wide.
		virtual void SetDopplerScale(float Scale) = 0;
		NODISCARD virtual float GetDopplerScale() const = 0;

		// Stops the output device without dropping voices; used when the app loses focus.
		virtual void SetSuspended(bool bSuspended) = 0;
		NODISCARD virtual bool IsSuspended() const = 0;

		virtual void SetMaxVoiceCount(uint32 MaxVoices) = 0;

		// Ramp applied to volume changes on newly started voices. Kills clicks on abrupt gain edits.
		virtual void SetVolumeSmoothing(float Milliseconds) = 0;

		// Rebuilds the output device. 0 means "device native" for any field. Applied on the pump, so
		// this returns before the device has actually changed.
		virtual void ApplyDeviceConfig(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames) = 0;
		NODISCARD virtual FAudioDeviceInfo GetDeviceInfo() const = 0;

		// Allocates a streaming PCM buffer (float32). Caller pushes samples via the returned stream;
		// playback is started by passing the stream to PlayProceduralStream.
		NODISCARD virtual TSharedPtr<FProceduralAudioStream> CreateProceduralStream(
			uint32 SampleRate, uint32 ChannelCount, uint32 BufferFrames) = 0;

		void SetMasterVolume(float Volume) { SetBusVolume(EAudioBus::Master, Volume); }
		NODISCARD float GetMasterVolume() const { return GetBusVolume(EAudioBus::Master); }

		NODISCARD FAudioHandle PlayAudio2D(const TSharedPtr<FAudioData>& Data,
			float Volume = 1.0f, float Pitch = 1.0f, bool bLooping = false, uint64 StartFrame = 0)
		{
			FAudioPlayParams Params;
			Params.Volume     = Volume;
			Params.Pitch      = Pitch;
			Params.bLooping   = bLooping;
			Params.StartFrame = StartFrame;
			return PlayAudio(Data, Params);
		}

		NODISCARD FAudioHandle PlayAudioAtLocation(const TSharedPtr<FAudioData>& Data, FVector3 Location,
			float Volume = 1.0f, float Pitch = 1.0f, float MinDistance = 1.0f, float MaxDistance = 50.0f,
			bool bLooping = false, uint64 StartFrame = 0)
		{
			FAudioPlayParams Params;
			Params.Volume                   = Volume;
			Params.Pitch                    = Pitch;
			Params.bLooping                 = bLooping;
			Params.bSpatialized             = true;
			Params.Position                 = Location;
			Params.StartFrame               = StartFrame;
			Params.Attenuation.MinDistance  = MinDistance;
			Params.Attenuation.MaxDistance  = MaxDistance;
			return PlayAudio(Data, Params);
		}

		NODISCARD FAudioHandle PlaySound2D(FStringView File, float Volume = 1.0f, float Pitch = 1.0f, bool bLooping = false)
		{
			FAudioPlayParams Params;
			Params.Volume   = Volume;
			Params.Pitch    = Pitch;
			Params.bLooping = bLooping;
			return PlayFile(File, Params);
		}

		NODISCARD FAudioHandle PlaySoundAtLocation(FStringView File, FVector3 Location,
			float Volume = 1.0f, float Pitch = 1.0f, float MinDistance = 1.0f, float MaxDistance = 50.0f, bool bLooping = false)
		{
			FAudioPlayParams Params;
			Params.Volume                   = Volume;
			Params.Pitch                    = Pitch;
			Params.bLooping                 = bLooping;
			Params.bSpatialized             = true;
			Params.Position                 = Location;
			Params.Attenuation.MinDistance  = MinDistance;
			Params.Attenuation.MaxDistance  = MaxDistance;
			return PlayFile(File, Params);
		}

		void UpdateListenerPosition(FVector3 Location, FQuat Rotation)
		{
			UpdateListener(0, Location, Rotation, FVector3(0.0f));
		}
	};
}
