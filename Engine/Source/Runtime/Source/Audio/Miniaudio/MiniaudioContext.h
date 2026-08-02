#pragma once

#include "Audio/AudioContext.h"
#include "Audio/AudioReverb.h"
#include "Audio/ProceduralAudioStream.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Threading/Thread.h"
#include "Core/Threading/Atomic.h"
#include "Memory/SmartPtr.h"
#include "MiniAudio/miniaudio.h"

namespace Lumina::Jobs { struct FCounter; }

namespace Lumina
{
	class FMiniaudioContext final : public IAudioContext
	{
	public:

		FMiniaudioContext();
		~FMiniaudioContext() override;

		// Kicks a single-in-flight pump job onto the task pool (drains commands + housekeeping).
		void Update() override;

		void* GetNative() const override { return (void*)&Engine; }

		FAudioHandle PlayAudio(const TSharedPtr<FAudioData>& Data, const FAudioPlayParams& Params) override;
		FAudioHandle PlayFile(FStringView File, const FAudioPlayParams& Params) override;
		FAudioHandle PlayProceduralStream(TSharedPtr<FProceduralAudioStream> Stream, const FAudioPlayParams& Params) override;

		void StopSound(FAudioHandle Handle, EAudioStopMode Mode, float FadeSeconds) override;
		void StopAllSounds(EAudioStopMode Mode, float FadeSeconds) override;

		void SetVolume(FAudioHandle Handle, float Volume) override;
		void SetPitch(FAudioHandle Handle, float Pitch) override;
		void SetLooping(FAudioHandle Handle, bool bLooping) override;
		void SetPosition(FAudioHandle Handle, FVector3 Position) override;
		void SetVelocity(FAudioHandle Handle, FVector3 Velocity) override;
		void SetDirection(FAudioHandle Handle, FVector3 Direction) override;
		void SetAttenuation(FAudioHandle Handle, const SAudioAttenuation& Attenuation) override;
		void SetMinMaxDistance(FAudioHandle Handle, float MinDistance, float MaxDistance) override;
		void SetPan(FAudioHandle Handle, float Pan) override;
		void SetPaused(FAudioHandle Handle, bool bPaused) override;
		void SetBus(FAudioHandle Handle, EAudioBus Bus) override;
		void SetPriority(FAudioHandle Handle, uint8 Priority) override;
		void SetOcclusion(FAudioHandle Handle, float Amount, float LowPassFrequency, float VolumeAttenuation) override;
		void SetLowPassCutoff(FAudioHandle Handle, float CutoffHz) override;
		void FadeTo(FAudioHandle Handle, float Volume, float Seconds) override;
		void SeekToFrame(FAudioHandle Handle, uint64 Frame) override;

		EAudioVoiceState GetVoiceState(FAudioHandle Handle) const override;
		uint64 GetPlaybackFrame(FAudioHandle Handle) const override;
		uint32 GetActiveVoiceCount() const override { return ActiveVoices.load(Atomic::MemoryOrderRelaxed); }
		uint32 GetMaxVoiceCount() const override { return MaxActiveVoices.load(Atomic::MemoryOrderRelaxed); }
		uint64 GetDroppedVoiceCount() const override { return DroppedVoices.load(Atomic::MemoryOrderRelaxed); }

		void UpdateListener(uint32 ListenerIndex, FVector3 Position, FQuat Rotation, FVector3 Velocity) override;
		void SetListenerEnabled(uint32 ListenerIndex, bool bEnabled) override;
		uint32 GetListenerCount() const override;

		void SetBusVolume(EAudioBus Bus, float Volume) override;
		float GetBusVolume(EAudioBus Bus) const override;
		void SetBusMuted(EAudioBus Bus, bool bMuted) override;
		bool IsBusMuted(EAudioBus Bus) const override;
		void SetBusPitch(EAudioBus Bus, float Pitch) override;
		void SetBusReverbSend(EAudioBus Bus, float SendLevel) override;
		float GetBusReverbSend(EAudioBus Bus) const override;

		void SetReverbParams(const FAudioReverbParams& Params) override;
		FAudioReverbParams GetReverbParams() const override;

		void SetDopplerScale(float Scale) override;
		float GetDopplerScale() const override { return DopplerScale.load(Atomic::MemoryOrderRelaxed); }

		void SetSuspended(bool bSuspended) override;
		bool IsSuspended() const override { return bSuspended.load(Atomic::MemoryOrderRelaxed); }

		void SetMaxVoiceCount(uint32 MaxVoices) override;
		void SetVolumeSmoothing(float Milliseconds) override;
		void ApplyDeviceConfig(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames) override;
		FAudioDeviceInfo GetDeviceInfo() const override;

		TSharedPtr<FProceduralAudioStream> CreateProceduralStream(uint32 SampleRate, uint32 ChannelCount, uint32 BufferFrames) override;

	private:

		static constexpr uint32 MaxVoiceSlots = 256;

		struct FActiveSound
		{
			FAudioHandle Handle;
			TVector<uint8> Bytes;
			ma_decoder Decoder;
			ma_sound Sound;
			ma_lpf_node LowPass;

			bool bInitialized = false;
			bool bDecoderInitialized = false;
			bool bLowPassInitialized = false;
			bool bPaused = false;
			bool bStopping = false;

			uint8 Priority = 128;
			EAudioBus Bus = EAudioBus::SFX;

			float BaseVolume = 1.0f;
			float OcclusionGain = 1.0f;
			float LowPassCutoff = 0.0f;
			float DopplerFactor = 1.0f;

			// Keeps the encoded bytes alive if the owning asset is unloaded mid-playback.
			TSharedPtr<FAudioData> Source;

			// Non-null for procedural voices; they are never auto-collected on end of data.
			TSharedPtr<FProceduralAudioStream> Procedural;
		};

		// Play requests carry shared pointers and a settings struct, so they ride a side queue rather
		// than the flat command queue. Drained before commands so a play + tweak in the same frame lands.
		struct FPendingPlay
		{
			FAudioHandle Handle;
			FAudioPlayParams Params;
			FString Path;
			TSharedPtr<FAudioData> Data;
			TSharedPtr<FProceduralAudioStream> Stream;
			uint32 StopEpoch = 0;
		};

		bool AcquireVoiceSlot(uint8 Priority, FAudioHandle& OutHandle);
		void ReleaseVoiceSlot(uint32 Slot, uint32 Generation);
		void ResetVoiceSlots();
		FActiveSound* FindSound(FAudioHandle Handle);

		void PumpOnce();
		static void PumpEntry(void* Arg, uint32 WorkerIndex);
		void ProcessCommand(const FAudioCommand& Cmd);
		void ProcessPendingPlay(FPendingPlay& Play);
		void CleanupFinishedSounds();
		void PublishVoiceState();

		bool CreateEngine(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames);
		void DestroyEngine();
		void CreateBusGroups();
		void DestroyBusGroups();
		void ApplyPendingDeviceConfig();
		void ApplyReverbRouting();

		void ApplyVoiceVolume(FActiveSound& Sound);
		void ApplyVoiceAttenuation(FActiveSound& Sound, const SAudioAttenuation& Attenuation);
		void ApplyVoiceLowPass(FActiveSound& Sound, float CutoffHz);
		bool AttachVoiceOutput(FActiveSound& Sound);
		void UninitSound(FActiveSound& Sound);

		ma_sound_group* GetBusGroup(EAudioBus Bus) { return &BusGroups[(uint32)Bus]; }

		ma_engine Engine;
		ma_allocation_callbacks AllocationCallbacks;

		ma_sound_group BusGroups[NumAudioBuses];
		ma_splitter_node BusSplitters[NumAudioBuses];
		bool bBusGroupInitialized[NumAudioBuses] = {};
		bool bBusSplitterInitialized[NumAudioBuses] = {};
		TAtomic<bool> bEngineInitialized{false};

		FAudioReverbNode Reverb;
		TAtomic<float> BusReverbSends[NumAudioBuses];
		TAtomic<bool> bReverbRoutingDirty{false};
		FAudioReverbParams PendingReverbParams;

		TAtomic<float> BusVolumes[NumAudioBuses];
		TAtomic<bool> bBusMuted[NumAudioBuses];

		TConcurrentQueue<FAudioCommand> CommandQueue;
		TConcurrentQueue<FPendingPlay> PendingPlays;

		TAtomic<bool>   bRunning{false};
		TAtomic<bool>   bPumpActive{false};
		Jobs::FCounter* PumpCounter = nullptr;

		TAtomic<uint32> NextGeneration{1};

		// Slot-indexed voice table. The pump owns Voices; the atomics are published for the game thread.
		TUniquePtr<FActiveSound> Voices[MaxVoiceSlots];
		TAtomic<uint8>  SlotState[MaxVoiceSlots];
		TAtomic<uint32> SlotGeneration[MaxVoiceSlots];
		TAtomic<uint8>  SlotPriority[MaxVoiceSlots];
		TAtomic<uint64> SlotFrame[MaxVoiceSlots];

		// Set by StopSound so a voice stopped before the pump ever started it doesn't leak through.
		TAtomic<bool>   SlotCancelled[MaxVoiceSlots];
		TConcurrentQueue<uint32> FreeSlots;
		FMutex SlotLock;

		TAtomic<uint32> ActiveVoices{0};
		TAtomic<uint32> MaxActiveVoices{128};
		TAtomic<uint64> DroppedVoices{0};

		// Bumped by StopAllSounds so plays queued before it are discarded rather than surviving it.
		TAtomic<uint32> StopEpoch{0};

		TAtomic<float> DopplerScale{1.0f};
		TAtomic<bool>  bDopplerDirty{false};
		TAtomic<bool>  bSuspended{false};
		TAtomic<uint32> VolumeSmoothFrames{0};

		TAtomic<bool>   bDeviceConfigDirty{false};
		TAtomic<uint32> PendingSampleRate{0};
		TAtomic<uint32> PendingChannels{0};
		TAtomic<uint32> PendingPeriodFrames{0};
	};
}
