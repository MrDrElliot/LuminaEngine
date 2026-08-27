#pragma once

#include "AudioContext.h"
#include "AudioDevice.h"
#include "AudioMixer.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Containers/BoundedQueue.h"
#include "Memory/SmartPtr.h"

namespace Lumina::Jobs { struct FCounter; }

namespace Lumina
{
	class FAudioGraphInstance;
	class FProceduralAudioStream;
	class IAudioSource;

	/** IAudioContext on the in-house mixer and platform device, with no third party audio library. */
	class RUNTIME_API FLuminaAudioContext final : public IAudioContext
	{
	public:

		FLuminaAudioContext();
		~FLuminaAudioContext() override;

		void Update() override;

		FAudioHandle PlayAudio(const TSharedPtr<FAudioData>& Data, const FAudioPlayParams& Params) override;
		FAudioHandle PlayFile(FStringView File, const FAudioPlayParams& Params) override;
		FAudioHandle PlayProceduralStream(TSharedPtr<FProceduralAudioStream> Stream, const FAudioPlayParams& Params) override;
		FAudioHandle PlayAudioGraph(TSharedPtr<FAudioGraphInstance> Instance, const FAudioPlayParams& Params) override;

		bool SetGraphFloatParameter(FAudioHandle Handle, const FName& Name, float Value) override;
		bool SetGraphIntParameter(FAudioHandle Handle, const FName& Name, int32 Value) override;
		bool SetGraphBoolParameter(FAudioHandle Handle, const FName& Name, bool Value) override;
		bool TriggerGraphParameter(FAudioHandle Handle, const FName& Name) override;
		float GetGraphFloatOutput(FAudioHandle Handle, const FName& Name) const override;
		uint32 GetGraphTriggerOutputCount(FAudioHandle Handle, const FName& Name) const override;

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
		uint32 GetListenerCount() const override { return FAudioMixer::MaxListeners; }

		void SetBusVolume(EAudioBus Bus, float Volume) override { Mixer.SetBusVolume(Bus, Volume); }
		float GetBusVolume(EAudioBus Bus) const override { return Mixer.GetBusVolume(Bus); }
		void SetBusMuted(EAudioBus Bus, bool bMuted) override { Mixer.SetBusMuted(Bus, bMuted); }
		bool IsBusMuted(EAudioBus Bus) const override { return Mixer.IsBusMuted(Bus); }
		void SetBusPitch(EAudioBus Bus, float Pitch) override { Mixer.SetBusPitch(Bus, Pitch); }
		void SetBusReverbSend(EAudioBus Bus, float SendLevel) override { Mixer.SetBusReverbSend(Bus, SendLevel); }
		float GetBusReverbSend(EAudioBus Bus) const override { return Mixer.GetBusReverbSend(Bus); }

		void SetReverbParams(const FAudioReverbParams& Params) override { Mixer.SetReverbParams(Params); }
		FAudioReverbParams GetReverbParams() const override { return Mixer.GetReverbParams(); }

		void SetDopplerScale(float Scale) override { Mixer.SetDopplerScale(Scale); }
		float GetDopplerScale() const override { return Mixer.GetDopplerScale(); }

		void SetSuspended(bool bInSuspended) override;
		bool IsSuspended() const override { return bSuspended.load(Atomic::MemoryOrderRelaxed); }

		void SetMaxVoiceCount(uint32 MaxVoices) override;
		void SetVolumeSmoothing(float Milliseconds) override { Mixer.SetVolumeSmoothing(Milliseconds); }

		void ApplyDeviceConfig(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames) override;
		FAudioDeviceInfo GetDeviceInfo() const override;

		TSharedPtr<FProceduralAudioStream> CreateProceduralStream(uint32 SampleRate, uint32 ChannelCount, uint32 BufferFrames) override;

	private:

		static constexpr uint32 MaxVoiceSlots = FAudioMixer::MaxVoices;

		/** A play request carrying shared pointers, so it rides a side queue rather than the flat commands. */
		struct FPendingPlay
		{
			FAudioHandle     Handle;
			FAudioPlayParams Params;
			FString          Path;

			TSharedPtr<FAudioData>             Data;
			TSharedPtr<FProceduralAudioStream> Stream;
			TSharedPtr<FAudioGraphInstance>    Graph;

			uint32 StopEpoch = 0;
		};

		/** A source unhooked from the mixer, freed once the device thread has finished the block using it. */
		struct FRetiredSource
		{
			TSharedPtr<IAudioSource> Source;
			uint64 RenderCountAtRetire = 0;
		};

		bool AcquireVoiceSlot(uint8 Priority, FAudioHandle& OutHandle);
		void ReleaseVoiceSlot(uint32 Slot, uint32 Generation);
		void ResetVoiceSlots();

		void PumpOnce();
		static void PumpEntry(void* Arg, uint32 WorkerIndex);

		void ProcessPendingPlay(FPendingPlay& Play);
		void ProcessCommand(const FAudioCommand& Cmd);
		void CollectFinishedVoices();
		void FlushRetiredSources(bool bForce);
		void RetireSlotSource(uint32 Slot);

		bool CreateDeviceAndMixer(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames);
		void DestroyDeviceAndMixer();
		void ApplyPendingDeviceConfig();

		void PostToMixer(const FAudioCommand& Cmd);

		TSharedPtr<FAudioGraphInstance> FindGraphInstance(FAudioHandle Handle) const;
		void ForgetGraphInstance(FAudioHandle Handle);
		void SweepGraphInstances();

		static uint64 MakeGraphKey(FAudioHandle Handle) { return ((uint64)Handle.Generation << 32) | Handle.Index; }

		FAudioMixer Mixer;
		TUniquePtr<IAudioDevice> Device;

		// The pump owns every source; the mixer only ever sees the raw pointer.
		TSharedPtr<IAudioSource> SlotSources[MaxVoiceSlots];

		// Render count when the start was queued, so a slot is not collected before the mixer picks it up.
		uint64 SlotStartRenderCount[MaxVoiceSlots] = {};
		TVector<FRetiredSource>  RetiredSources;

		TBoundedMPSCQueue<FAudioCommand> CommandQueue;
		TBoundedMPSCQueue<FPendingPlay>  PendingPlays;
		TBoundedMPMCQueue<uint32, MaxVoiceSlots> FreeSlots;

		TAtomic<uint8>  SlotState[MaxVoiceSlots];
		TAtomic<uint32> SlotGeneration[MaxVoiceSlots];
		TAtomic<uint8>  SlotPriority[MaxVoiceSlots];
		TAtomic<bool>   SlotCanceled[MaxVoiceSlots];
		FMutex SlotLock;

		mutable FMutex GraphInstanceLock;
		THashMap<uint64, TSharedPtr<FAudioGraphInstance>> GraphInstances;

		TAtomic<bool>   bRunning{false};
		TAtomic<bool>   bPumpActive{false};
		Jobs::FCounter* PumpCounter = nullptr;

		TAtomic<uint32> NextGeneration{1};
		TAtomic<uint32> ActiveVoices{0};
		TAtomic<uint32> MaxActiveVoices{128};
		TAtomic<uint64> DroppedVoices{0};
		TAtomic<uint32> StopEpoch{0};

		TAtomic<bool> bSuspended{false};

		TAtomic<bool>   bDeviceConfigDirty{false};
		TAtomic<uint32> PendingSampleRate{0};
		TAtomic<uint32> PendingChannels{0};
		TAtomic<uint32> PendingPeriodFrames{0};
	};
}
