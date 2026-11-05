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
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
	class FAudioGraphInstance;
	class FProceduralAudioStream;
	class IAudioSource;

	class RUNTIME_API FLuminaAudioContext final : public IAudioContext
	{
	public:

		FLuminaAudioContext();
		~FLuminaAudioContext() override;
		FLuminaAudioContext(const FLuminaAudioContext&) = delete;
		FLuminaAudioContext& operator=(const FLuminaAudioContext&) = delete;
		FLuminaAudioContext(FLuminaAudioContext&&) = delete;
		FLuminaAudioContext& operator=(FLuminaAudioContext&&) = delete;

		void Update() override;

		SAudioHandle PlayAudio(const TSharedPtr<FAudioData>& Data, const SAudioPlayParams& Params) override;
		SAudioHandle PlayFile(FStringView File, const SAudioPlayParams& Params) override;
		SAudioHandle PlayProceduralStream(TSharedPtr<FProceduralAudioStream> Stream, const SAudioPlayParams& Params) override;
		SAudioHandle PlayAudioGraph(TSharedPtr<FAudioGraphInstance> Instance, const SAudioPlayParams& Params) override;

		bool SetGraphFloatParameter(SAudioHandle Handle, const FName& Name, float Value) override;
		bool SetGraphIntParameter(SAudioHandle Handle, const FName& Name, int32 Value) override;
		bool SetGraphBoolParameter(SAudioHandle Handle, const FName& Name, bool Value) override;
		bool TriggerGraphParameter(SAudioHandle Handle, const FName& Name) override;
		float GetGraphFloatOutput(SAudioHandle Handle, const FName& Name) const override;
		uint32 GetGraphTriggerOutputCount(SAudioHandle Handle, const FName& Name) const override;

		void StopSound(SAudioHandle Handle, EAudioStopMode Mode, float FadeSeconds) override;
		void StopAllSounds(EAudioStopMode Mode, float FadeSeconds) override;

		void SetVolume(SAudioHandle Handle, float Volume) override;
		void SetPitch(SAudioHandle Handle, float Pitch) override;
		void SetLooping(SAudioHandle Handle, bool bLooping) override;
		void SetPosition(SAudioHandle Handle, FVector3 Position) override;
		void SetVelocity(SAudioHandle Handle, FVector3 Velocity) override;
		void SetDirection(SAudioHandle Handle, FVector3 Direction) override;
		void SetAttenuation(SAudioHandle Handle, const SAudioAttenuation& Attenuation) override;
		void SetMinMaxDistance(SAudioHandle Handle, float MinDistance, float MaxDistance) override;
		void SetPan(SAudioHandle Handle, float Pan) override;
		void SetPaused(SAudioHandle Handle, bool bPaused) override;
		void SetBus(SAudioHandle Handle, EAudioBus Bus) override;
		void SetPriority(SAudioHandle Handle, uint8 Priority) override;
		void SetOcclusion(SAudioHandle Handle, float Amount, float LowPassFrequency, float VolumeAttenuation) override;
		void SetLowPassCutoff(SAudioHandle Handle, float CutoffHz) override;
		void FadeTo(SAudioHandle Handle, float Volume, float Seconds) override;
		void SeekToFrame(SAudioHandle Handle, uint64 Frame) override;

		EAudioVoiceState GetVoiceState(SAudioHandle Handle) const override;
		uint64 GetPlaybackFrame(SAudioHandle Handle) const override;
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
		bool BeginOfflineRender(uint32& OutSampleRate, uint32& OutChannels) override;
		uint32 RenderOffline(float* OutInterleaved, uint32 FrameCount) override;
		void EndOfflineRender() override;

		TSharedPtr<FProceduralAudioStream> CreateProceduralStream(uint32 SampleRate, uint32 ChannelCount, uint32 BufferFrames) override;

	private:

		static constexpr uint32 MaxVoiceSlots = FAudioMixer::MaxVoices;

		// A play request carrying shared pointers, so it rides a side queue rather than the flat commands.
		struct FPendingPlay
		{
			SAudioHandle     Handle;
			SAudioPlayParams Params;
			FString          Path;

			TSharedPtr<FAudioData>             Data;
			TSharedPtr<FProceduralAudioStream> Stream;
			TSharedPtr<FAudioGraphInstance>    Graph;

			uint32 StopEpoch = 0;
		};

		// A source unhooked from its slot, freed only once the device thread can no longer be reading it.
		struct FRetiredSource
		{
			TSharedPtr<IAudioSource> Source;
			uint64 RenderCountAtRetire = 0;
			uint32 Slot = 0;
			uint32 Generation = 0;
		};

		bool AcquireVoiceSlot(uint8 Priority, SAudioHandle& OutHandle);
		void ReleaseVoiceSlot(uint32 Slot, uint32 Generation);
		void ResetVoiceSlots();

		void PumpOnce();
		static Jobs::TTask<void> PumpTask(FLuminaAudioContext* Self);

		void ProcessPendingPlay(FPendingPlay& Play);
		void ProcessCommand(const FAudioCommand& Cmd);
		void CollectFinishedVoices();
		void FlushRetiredSources(bool bForce);
		void RetireSlotSource(uint32 Slot);

		bool CreateDeviceAndMixer(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames);
		void DestroyDeviceAndMixer();
		void ApplyPendingDeviceConfig();

		void PostToMixer(const FAudioCommand& Cmd);

		TSharedPtr<FAudioGraphInstance> FindGraphInstance(SAudioHandle Handle) const;
		void ForgetGraphInstance(SAudioHandle Handle);
		void SweepGraphInstances();

		static uint64 MakeGraphKey(SAudioHandle Handle) { return ((uint64)Handle.Generation << 32) | Handle.Index; }

		// Sits between the device and the mixer, so an offline render can take the mixer from the realtime thread.
		class FDeviceRouter final : public IAudioRenderCallback
		{
		public:
			explicit FDeviceRouter(FAudioMixer& InMixer) : Mixer(InMixer) {}
			void RenderAudio(float* OutInterleaved, uint32 FrameCount) override;

			FAudioMixer&    Mixer;
			TAtomic<bool>   bOffline{false};
			TAtomic<uint32> InsideRender{0};
		};

		FAudioMixer Mixer;
		FDeviceRouter Router{Mixer};
		TUniquePtr<IAudioDevice> Device;

		// The pump owns every source; the mixer only ever sees the raw pointer.
		TSharedPtr<IAudioSource> SlotSources[MaxVoiceSlots];

		// The voice each source was started for, which a priority takeover leaves behind SlotGeneration.
		uint32 SlotSourceGeneration[MaxVoiceSlots] = {};
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
		Jobs::FTaskEvent PumpDone;

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
