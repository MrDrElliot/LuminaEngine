#pragma once

#include "AudioCommand.h"
#include "AudioDevice.h"
#include "AudioReverb.h"
#include "AudioTypes.h"
#include "Containers/BoundedQueue.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Atomic.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
	class IAudioSource;

	/** Everything the pump hands the mixer to start one voice. The source stays owned by the caller. */
	struct FMixerVoiceDesc
	{
		uint32           Slot = 0;
		uint32           Generation = 0;
		IAudioSource*    Source = nullptr;
		bool             bGenerated = false;
		FAudioPlayParams Params;
	};

	/** One listener's pose, published by the pump and sampled by the mixer once per block. */
	struct FMixerListener
	{
		FVector3 Position = FVector3(0.0f);
		FVector3 Forward  = FVector3(0.0f, 0.0f, 1.0f);
		FVector3 Up       = FVector3(0.0f, 1.0f, 0.0f);
		FVector3 Right    = FVector3(1.0f, 0.0f, 0.0f);
		FVector3 Velocity = FVector3(0.0f);
		bool     bEnabled = false;
	};

	/** Block mixer rendering on the device thread; the pump reaches it only through queues and atomics. */
	class RUNTIME_API FAudioMixer final : public IAudioRenderCallback
	{
	public:

		static constexpr uint32 MaxVoices    = 256;
		static constexpr uint32 MaxChannels  = 8;
		static constexpr uint32 MaxListeners = 4;

		// Matches kAudioGraphBlockFrames, so a graph voice never has to re-chunk what it renders.
		static constexpr uint32 BlockFrames = 256;

		// Bounds the resampler scratch, so a wild pitch cannot ask for an unbounded source read.
		static constexpr float MinRatio = 0.0625f;
		static constexpr float MaxRatio = 8.0f;

		FAudioMixer() = default;
		~FAudioMixer() override;

		FAudioMixer(const FAudioMixer&) = delete;
		FAudioMixer& operator=(const FAudioMixer&) = delete;

		bool Initialize(uint32 InSampleRate, uint32 InChannels);
		void Shutdown();

		bool IsInitialized() const { return bInitialized.load(Atomic::MemoryOrderAcquire); }

		uint32 GetSampleRate() const { return SampleRate; }
		uint32 GetChannelCount() const { return NumChannels; }

		void RenderAudio(float* Out, uint32 FrameCount) override;

		//~ Pump side. Every call here is lock free and allocation free.
		bool StartVoice(const FMixerVoiceDesc& Desc);
		bool PostCommand(const FAudioCommand& Cmd);

		/** Free means the mixer has let go of the slot's source and the pump may release it. */
		EAudioVoiceState GetSlotState(uint32 Slot) const;
		uint64 GetSlotFrame(uint32 Slot) const;

		uint32 GetActiveVoiceCount() const { return ActiveVoices.load(Atomic::MemoryOrderRelaxed); }

		// Bumped once per device callback, so the pump can tell when an unhooked source is safe to free.
		uint64 GetRenderCount() const { return RenderCount.load(Atomic::MemoryOrderAcquire); }

		void SetBusVolume(EAudioBus Bus, float Volume);
		float GetBusVolume(EAudioBus Bus) const;
		void SetBusMuted(EAudioBus Bus, bool bMuted);
		bool IsBusMuted(EAudioBus Bus) const;
		void SetBusPitch(EAudioBus Bus, float Pitch);
		void SetBusReverbSend(EAudioBus Bus, float Send);
		float GetBusReverbSend(EAudioBus Bus) const;

		void SetReverbParams(const FAudioReverbParams& Params);
		FAudioReverbParams GetReverbParams() const;

		void SetDopplerScale(float Scale);
		float GetDopplerScale() const { return DopplerScale.load(Atomic::MemoryOrderRelaxed); }

		void SetListener(uint32 Index, FVector3 Position, FQuat Rotation, FVector3 Velocity);
		void SetListenerEnabled(uint32 Index, bool bEnabled);
		uint32 GetListenerCount() const { return MaxListeners; }

		void SetVolumeSmoothing(float Milliseconds);
		void SetMaxVoiceCount(uint32 MaxCount);

	private:

		/** Mixer-owned voice state. Touched only on the device thread once the voice is live. */
		struct FVoice
		{
			IAudioSource* Source = nullptr;

			uint32 Generation = 0;
			bool bActive = false;
			bool bPaused = false;
			bool bSpatialized = false;
			bool bGenerated = false;

			EAudioBus Bus = EAudioBus::SFX;

			float BaseVolume = 1.0f;
			float Pitch = 1.0f;
			float OcclusionGain = 1.0f;

			FVector3 Position  = FVector3(0.0f);
			FVector3 Velocity  = FVector3(0.0f);
			FVector3 Direction = FVector3(0.0f, 0.0f, 1.0f);

			SAudioAttenuation Attenuation;

			double ResamplePos = 0.0;
			float  PrevFrame[MaxChannels] = {};
			float  NextFrame[MaxChannels] = {};
			bool   bPrimed = false;
			bool   bSourceDrained = false;
			bool   bHeldLast = false;

			float CurrentGain[MaxChannels] = {};
			bool  bGainPrimed = false;

			float LowPassCutoff = 0.0f;
			float LowPassState[MaxChannels] = {};

			float FadeGain = 1.0f;
			float FadeTarget = 1.0f;
			float FadeStep = 0.0f;
			bool  bStopAfterFade = false;

			uint64 StartDelayFrames = 0;
			uint64 PlayedFrames = 0;
		};

		void RenderBlock(float* Out, uint32 Frames);
		void DrainStarts();
		void DrainCommands();
		void ApplyCommand(const FAudioCommand& Cmd);

		void RenderVoice(FVoice& Voice, uint32 Frames);
		uint32 ResampleVoice(FVoice& Voice, uint32 Frames, float Ratio);
		void ComputeVoiceGains(const FVoice& Voice, float* OutGains) const;
		float ComputeVoicePitch(const FVoice& Voice) const;

		void DeactivateVoice(FVoice& Voice, uint32 Slot);
		void PublishSlot(uint32 Slot, EAudioVoiceState State, uint64 Frame);

		void ReadListener(uint32 Index, FMixerListener& Out) const;
		void SelectListener(const FVector3& Position, FMixerListener& Out) const;

		uint32 SampleRate  = 48000;
		uint32 NumChannels = 2;

		FVoice Voices[MaxVoices];

		TVector<float> BusAccum;
		TVector<float> MasterAccum;
		TVector<float> ReverbInput;
		TVector<float> ReverbOutput;
		TVector<float> VoiceScratch;
		TVector<float> SourceScratch;

		FAudioReverb Reverb;

		TBoundedMPSCQueue<FMixerVoiceDesc> StartQueue;
		TBoundedMPSCQueue<FAudioCommand>   CommandQueue;

		TAtomic<uint8>  SlotState[MaxVoices];
		TAtomic<uint64> SlotFrame[MaxVoices];

		TAtomic<float> BusVolumes[NumAudioBuses];
		TAtomic<bool>  BusMuted[NumAudioBuses];
		TAtomic<float> BusPitches[NumAudioBuses];
		TAtomic<float> BusReverbSends[NumAudioBuses];

		// Seqlock per listener; odd means a write is in flight, so the reader retries.
		FMixerListener  Listeners[MaxListeners];
		TAtomic<uint32> ListenerVersion[MaxListeners];

		TAtomic<uint64> RenderCount{0};
		TAtomic<uint32> ActiveVoices{0};
		TAtomic<uint32> MaxActiveVoices{128};
		TAtomic<float>  DopplerScale{1.0f};
		TAtomic<uint32> GainRampFrames{0};
		TAtomic<bool>   bInitialized{false};
		TAtomic<bool>   bReverbDirty{false};

		FAudioReverbParams PendingReverbParams;
	};
}
