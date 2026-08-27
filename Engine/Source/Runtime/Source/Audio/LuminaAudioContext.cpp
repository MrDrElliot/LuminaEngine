#include "RuntimePCH.h"
#include "LuminaAudioContext.h"

#include "AudioSource.h"
#include "FileSystem/FileSystem.h"
#include "Graph/AudioGraphInstance.h"
#include "Log/Log.h"
#include "Memory/MemoryTracking.h"
#include "ProceduralAudioStream.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

namespace Lumina
{
	namespace
	{
		constexpr uint32 kMaxPlaysPerPump    = 32;
		constexpr uint32 kMaxCommandsPerPump = 512;

		// A source is freed once the device thread has started a later block than the one that retired it.
		constexpr uint64 kRetireRenderDelay = 2;

		constexpr uint32 kCommandQueueCapacity = 4096;
		constexpr uint32 kPendingPlayCapacity  = 256;
	}

	FLuminaAudioContext::FLuminaAudioContext()
	{
		LUMINA_MEMORY_SCOPE("Audio");

		CommandQueue.Initialize(kCommandQueueCapacity);
		PendingPlays.Initialize(kPendingPlayCapacity);
		FreeSlots.Initialize(MaxVoiceSlots);

		ResetVoiceSlots();

		if (CreateDeviceAndMixer(0, 0, 0))
		{
			bRunning.store(true, Atomic::MemoryOrderRelease);
		}
	}

	FLuminaAudioContext::~FLuminaAudioContext()
	{
		bRunning.store(false, Atomic::MemoryOrderRelease);

		if (PumpCounter != nullptr)
		{
			Jobs::WaitForCounter(PumpCounter);
			Jobs::FreeCounter(PumpCounter);
			PumpCounter = nullptr;
		}

		DestroyDeviceAndMixer();

		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			SlotSources[Slot].reset();
		}
		RetiredSources.clear();
	}

	bool FLuminaAudioContext::CreateDeviceAndMixer(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames)
	{
		FAudioDeviceConfig DeviceConfig;
		DeviceConfig.SampleRate   = SampleRate;
		DeviceConfig.Channels     = Channels;
		DeviceConfig.PeriodFrames = PeriodFrames;

		// The mixer opens first, because the backend starts calling back the moment the device does.
		if (!Mixer.Initialize(SampleRate != 0 ? SampleRate : 48000, Channels != 0 ? Channels : 2))
		{
			LOG_ERROR("[Audio] mixer initialization failed");
			return false;
		}

		Device = Audio::CreateDevice(DeviceConfig, &Mixer);
		if (!Device)
		{
			LOG_WARN("[Audio] no output device; the mixer will run silent");
			return true;
		}

		if (Device->GetSampleRate() != Mixer.GetSampleRate() || Device->GetChannelCount() != Mixer.GetChannelCount())
		{
			// The endpoint settled on a different format, so the mixer is rebuilt to match it exactly.
			Mixer.Shutdown();
			if (!Mixer.Initialize(Device->GetSampleRate(), Device->GetChannelCount()))
			{
				LOG_ERROR("[Audio] mixer could not adopt the device format");
				Device.reset();
				return false;
			}
		}

		return true;
	}

	void FLuminaAudioContext::DestroyDeviceAndMixer()
	{
		if (Device)
		{
			Device->Stop();
			Device.reset();
		}

		Mixer.Shutdown();
		FlushRetiredSources(true);
	}

	void FLuminaAudioContext::ResetVoiceSlots()
	{
		FScopeLock Lock(SlotLock);

		uint32 Discard = 0;
		while (FreeSlots.TryDequeue(Discard))
		{
		}

		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			SlotState[Slot].store((uint8)EAudioVoiceState::Free, Atomic::MemoryOrderRelaxed);
			SlotGeneration[Slot].store(0, Atomic::MemoryOrderRelaxed);
			SlotPriority[Slot].store(0, Atomic::MemoryOrderRelaxed);
			SlotCanceled[Slot].store(false, Atomic::MemoryOrderRelaxed);
			FreeSlots.Enqueue(Slot);
		}

		ActiveVoices.store(0, Atomic::MemoryOrderRelease);
	}

	bool FLuminaAudioContext::AcquireVoiceSlot(uint8 Priority, FAudioHandle& OutHandle)
	{
		uint32 Slot = 0;
		bool bTookFreeSlot = false;

		if (ActiveVoices.load(Atomic::MemoryOrderRelaxed) < MaxActiveVoices.load(Atomic::MemoryOrderRelaxed) && FreeSlots.TryDequeue(Slot))
		{
			bTookFreeSlot = true;
		}
		else
		{
			FScopeLock Lock(SlotLock);

			if (ActiveVoices.load(Atomic::MemoryOrderRelaxed) < MaxActiveVoices.load(Atomic::MemoryOrderRelaxed) && FreeSlots.TryDequeue(Slot))
			{
				bTookFreeSlot = true;
			}
			else
			{
				// Steal the quietest-priority voice strictly below ours; ties keep the older voice.
				uint32 Victim = MaxVoiceSlots;
				uint8 VictimPriority = Priority;
				for (uint32 i = 0; i < MaxVoiceSlots; ++i)
				{
					if (SlotState[i].load(Atomic::MemoryOrderRelaxed) == (uint8)EAudioVoiceState::Free)
					{
						continue;
					}
					const uint8 SlotPri = SlotPriority[i].load(Atomic::MemoryOrderRelaxed);
					if (SlotPri < VictimPriority)
					{
						VictimPriority = SlotPri;
						Victim = i;
					}
				}

				if (Victim == MaxVoiceSlots)
				{
					DroppedVoices.fetch_add(1, Atomic::MemoryOrderRelaxed);
					return false;
				}

				Slot = Victim;
			}
		}

		uint32 Generation = NextGeneration.fetch_add(1, Atomic::MemoryOrderRelaxed);
		if (Generation == 0)
		{
			Generation = NextGeneration.fetch_add(1, Atomic::MemoryOrderRelaxed);
		}

		SlotCanceled[Slot].store(false, Atomic::MemoryOrderRelaxed);
		SlotPriority[Slot].store(Priority, Atomic::MemoryOrderRelaxed);
		SlotGeneration[Slot].store(Generation, Atomic::MemoryOrderRelaxed);
		SlotState[Slot].store((uint8)EAudioVoiceState::Playing, Atomic::MemoryOrderRelease);

		if (bTookFreeSlot)
		{
			ActiveVoices.fetch_add(1, Atomic::MemoryOrderRelaxed);
		}

		OutHandle.Generation = Generation;
		OutHandle.Index      = Slot;
		return true;
	}

	void FLuminaAudioContext::ReleaseVoiceSlot(uint32 Slot, uint32 Generation)
	{
		FScopeLock Lock(SlotLock);

		// A newer voice may already own this slot through a priority takeover; leave its claim intact.
		if (SlotGeneration[Slot].load(Atomic::MemoryOrderRelaxed) != Generation)
		{
			return;
		}

		SlotState[Slot].store((uint8)EAudioVoiceState::Free, Atomic::MemoryOrderRelease);
		SlotPriority[Slot].store(0, Atomic::MemoryOrderRelaxed);
		ActiveVoices.fetch_sub(1, Atomic::MemoryOrderRelaxed);
		FreeSlots.Enqueue(Slot);
	}

	void FLuminaAudioContext::RetireSlotSource(uint32 Slot)
	{
		if (!SlotSources[Slot])
		{
			return;
		}

		FRetiredSource Retired;
		Retired.Source = Move(SlotSources[Slot]);
		Retired.RenderCountAtRetire = Mixer.GetRenderCount();

		SlotSources[Slot].reset();
		RetiredSources.push_back(Move(Retired));
	}

	void FLuminaAudioContext::FlushRetiredSources(bool bForce)
	{
		if (RetiredSources.empty())
		{
			return;
		}

		// With no device running nothing can still be reading them, so they go immediately.
		const bool bDeviceIdle = !Device || !Device->IsRunning() || !Mixer.IsInitialized();
		const uint64 Now = Mixer.GetRenderCount();

		for (size_t i = RetiredSources.size(); i > 0; --i)
		{
			const size_t Index = i - 1;
			if (bForce || bDeviceIdle || Now > RetiredSources[Index].RenderCountAtRetire + kRetireRenderDelay)
			{
				RetiredSources.erase(RetiredSources.begin() + (ptrdiff_t)Index);
			}
		}
	}

	void FLuminaAudioContext::PostToMixer(const FAudioCommand& Cmd)
	{
		if (!CommandQueue.TryEnqueue(Cmd))
		{
			LOG_WARN_ONCE("Audio: the command queue is full; dropping a voice command");
		}
	}

	void FLuminaAudioContext::Update()
	{
		SweepGraphInstances();

		if (!bRunning.load(Atomic::MemoryOrderAcquire) || !Jobs::IsInitialized())
		{
			return;
		}

		if (PumpCounter == nullptr)
		{
			PumpCounter = Jobs::AllocCounter(0);
		}

		bool Expected = false;
		if (bPumpActive.compare_exchange_strong(Expected, true, Atomic::MemoryOrderAcqRel))
		{
			Jobs::RunJob(&FLuminaAudioContext::PumpEntry, this, Jobs::EJobPriority::Normal, PumpCounter, "Audio::Pump", true);
		}
	}

	void FLuminaAudioContext::PumpEntry(void* Arg, uint32)
	{
		FLuminaAudioContext* Self = static_cast<FLuminaAudioContext*>(Arg);
		Self->PumpOnce();
		Self->bPumpActive.store(false, Atomic::MemoryOrderRelease);
	}

	void FLuminaAudioContext::PumpOnce()
	{
		LUMINA_PROFILE_SECTION_COLORED("Audio::Pump", tracy::Color::Orange);

		// A lost endpoint (device unplugged, default output moved) rebuilds rather than going silent.
		if (Device && Device->NeedsRestart())
		{
			bDeviceConfigDirty.store(true, Atomic::MemoryOrderRelease);
		}

		ApplyPendingDeviceConfig();

		if (!Mixer.IsInitialized())
		{
			return;
		}

		FPendingPlay Play;
		uint32 PlaysProcessed = 0;
		while (PlaysProcessed < kMaxPlaysPerPump && PendingPlays.TryDequeue(Play))
		{
			ProcessPendingPlay(Play);
			++PlaysProcessed;
		}

		FAudioCommand Cmd;
		uint32 CommandsProcessed = 0;
		while (CommandsProcessed < kMaxCommandsPerPump && CommandQueue.TryDequeue(Cmd))
		{
			ProcessCommand(Cmd);
			++CommandsProcessed;
		}

		CollectFinishedVoices();
		FlushRetiredSources(false);
	}

	void FLuminaAudioContext::ProcessCommand(const FAudioCommand& Cmd)
	{
		if (Cmd.Type == EAudioCommandType::UpdateListener)
		{
			// Written here rather than in the mixer, because the seqlock needs a single writer.
			Mixer.SetListener(Cmd.ByteValue, Cmd.Vector, Cmd.Rotation, Cmd.VectorB);
			return;
		}

		if (Cmd.Type == EAudioCommandType::SetPriority)
		{
			if (Cmd.Handle.Index < MaxVoiceSlots &&
				SlotGeneration[Cmd.Handle.Index].load(Atomic::MemoryOrderRelaxed) == Cmd.Handle.Generation)
			{
				SlotPriority[Cmd.Handle.Index].store(Cmd.ByteValue, Atomic::MemoryOrderRelaxed);
			}
			return;
		}

		if (Cmd.Type == EAudioCommandType::StopAll)
		{
			Mixer.PostCommand(Cmd);
			return;
		}

		if (Cmd.Type == EAudioCommandType::StopSound && Cmd.Handle.Index < MaxVoiceSlots)
		{
			SlotCanceled[Cmd.Handle.Index].store(true, Atomic::MemoryOrderRelaxed);
		}

		Mixer.PostCommand(Cmd);
	}

	void FLuminaAudioContext::ProcessPendingPlay(FPendingPlay& Play)
	{
		LUMINA_MEMORY_SCOPE("Audio");

		const uint32 Slot = Play.Handle.Index;

		// The voice was stopped or flushed before the pump ever saw it.
		if (Play.StopEpoch != StopEpoch.load(Atomic::MemoryOrderRelaxed) || SlotCanceled[Slot].load(Atomic::MemoryOrderRelaxed))
		{
			ReleaseVoiceSlot(Slot, Play.Handle.Generation);
			return;
		}

		// A priority takeover evicts this slot's previous occupant, whose source outlives the swap.
		RetireSlotSource(Slot);

		TSharedPtr<IAudioSource> Source;
		bool bGenerated = false;

		if (Play.Graph)
		{
			Source = MakeShared<FAudioGraphSource>(Play.Graph);
			bGenerated = true;
		}
		else if (Play.Stream)
		{
			Source = MakeShared<FProceduralAudioSource>(Play.Stream);
			bGenerated = true;
		}
		else
		{
			TSharedPtr<FWaveAudioSource> Wave = MakeShared<FWaveAudioSource>();
			bool bOpened = false;

			if (Play.Data)
			{
				bOpened = Wave->Open(Play.Data);
			}
			else
			{
				TVector<uint8> Bytes;
				if (!VFS::ReadFile(Bytes, FStringView(Play.Path)))
				{
					LOG_WARN("Audio: failed to read file '{}'", Play.Path.c_str());
					ReleaseVoiceSlot(Slot, Play.Handle.Generation);
					return;
				}
				bOpened = Wave->Open(Move(Bytes));
			}

			if (!bOpened)
			{
				LOG_WARN("Audio: failed to decode audio data");
				ReleaseVoiceSlot(Slot, Play.Handle.Generation);
				return;
			}

			Source = Wave;
		}

		FMixerVoiceDesc Desc;
		Desc.Slot       = Slot;
		Desc.Generation = Play.Handle.Generation;
		Desc.Source     = Source.get();
		Desc.bGenerated = bGenerated;
		Desc.Params     = Play.Params;

		// Published before the start is queued, so the device thread never sees a slot without its source.
		SlotSources[Slot] = Move(Source);
		SlotStartRenderCount[Slot] = Mixer.GetRenderCount();

		if (!Mixer.StartVoice(Desc))
		{
			LOG_WARN("Audio: the mixer start queue is full; dropping a voice");
			RetireSlotSource(Slot);
			ReleaseVoiceSlot(Slot, Play.Handle.Generation);
			DroppedVoices.fetch_add(1, Atomic::MemoryOrderRelaxed);
			return;
		}

		SlotState[Slot].store(
			Play.Params.bStartPaused ? (uint8)EAudioVoiceState::Paused : (uint8)EAudioVoiceState::Playing,
			Atomic::MemoryOrderRelease);
	}

	void FLuminaAudioContext::CollectFinishedVoices()
	{
		// With no device nothing will ever drain the start queue, so a queued voice is collected at once.
		const bool bDeviceIdle = !Device || !Device->IsRunning();
		const uint64 Now = Mixer.GetRenderCount();

		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			if (!SlotSources[Slot])
			{
				continue;
			}

			// A slot reads Free until the device thread drains the start queue, which is one render away.
			if (!bDeviceIdle && Now <= SlotStartRenderCount[Slot])
			{
				continue;
			}

			const EAudioVoiceState MixerState = Mixer.GetSlotState(Slot);
			if (MixerState != EAudioVoiceState::Free)
			{
				SlotState[Slot].store((uint8)MixerState, Atomic::MemoryOrderRelease);
				continue;
			}

			const uint32 Generation = SlotGeneration[Slot].load(Atomic::MemoryOrderRelaxed);

			RetireSlotSource(Slot);
			ForgetGraphInstance(FAudioHandle{ Generation, Slot });
			ReleaseVoiceSlot(Slot, Generation);
		}
	}

	void FLuminaAudioContext::ApplyPendingDeviceConfig()
	{
		if (!bDeviceConfigDirty.exchange(false, Atomic::MemoryOrderAcqRel))
		{
			return;
		}

		const uint32 SampleRate   = PendingSampleRate.load(Atomic::MemoryOrderRelaxed);
		const uint32 Channels     = PendingChannels.load(Atomic::MemoryOrderRelaxed);
		const uint32 PeriodFrames = PendingPeriodFrames.load(Atomic::MemoryOrderRelaxed);

		if (Device && Device->GetSampleRate() == (SampleRate != 0 ? SampleRate : Device->GetSampleRate())
			&& Device->GetChannelCount() == (Channels != 0 ? Channels : Device->GetChannelCount())
			&& !Device->NeedsRestart())
		{
			return;
		}

		LOG_INFO("[Audio] rebuilding the output device");

		StopAllSounds(EAudioStopMode::Immediate, 0.0f);

		FAudioCommand Flush;
		while (CommandQueue.TryDequeue(Flush))
		{
		}

		DestroyDeviceAndMixer();

		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			SlotSources[Slot].reset();
		}
		ResetVoiceSlots();

		if (!CreateDeviceAndMixer(SampleRate, Channels, PeriodFrames))
		{
			bRunning.store(false, Atomic::MemoryOrderRelease);
		}
	}

	FAudioHandle FLuminaAudioContext::PlayAudio(const TSharedPtr<FAudioData>& Data, const FAudioPlayParams& Params)
	{
		if (!Data || Data->Bytes.empty())
		{
			return FAudioHandle::Invalid();
		}

		FAudioHandle Handle;
		if (!AcquireVoiceSlot(Params.Priority, Handle))
		{
			return FAudioHandle::Invalid();
		}

		FPendingPlay Play;
		Play.Handle    = Handle;
		Play.Params    = Params;
		Play.Data      = Data;
		Play.StopEpoch = StopEpoch.load(Atomic::MemoryOrderRelaxed);

		if (!PendingPlays.TryEnqueue(Play))
		{
			LOG_WARN_ONCE("Audio: the pending play queue is full; dropping a voice");
			ReleaseVoiceSlot(Handle.Index, Handle.Generation);
			DroppedVoices.fetch_add(1, Atomic::MemoryOrderRelaxed);
			return FAudioHandle::Invalid();
		}

		return Handle;
	}

	FAudioHandle FLuminaAudioContext::PlayFile(FStringView File, const FAudioPlayParams& Params)
	{
		if (File.empty())
		{
			return FAudioHandle::Invalid();
		}

		FAudioHandle Handle;
		if (!AcquireVoiceSlot(Params.Priority, Handle))
		{
			return FAudioHandle::Invalid();
		}

		FPendingPlay Play;
		Play.Handle    = Handle;
		Play.Params    = Params;
		Play.Path      = FString(File);
		Play.StopEpoch = StopEpoch.load(Atomic::MemoryOrderRelaxed);

		if (!PendingPlays.TryEnqueue(Play))
		{
			LOG_WARN_ONCE("Audio: the pending play queue is full; dropping a voice");
			ReleaseVoiceSlot(Handle.Index, Handle.Generation);
			DroppedVoices.fetch_add(1, Atomic::MemoryOrderRelaxed);
			return FAudioHandle::Invalid();
		}

		return Handle;
	}

	FAudioHandle FLuminaAudioContext::PlayProceduralStream(TSharedPtr<FProceduralAudioStream> Stream, const FAudioPlayParams& Params)
	{
		if (!Stream || !Stream->IsValid())
		{
			return FAudioHandle::Invalid();
		}

		FAudioHandle Handle;
		if (!AcquireVoiceSlot(Params.Priority, Handle))
		{
			return FAudioHandle::Invalid();
		}

		FPendingPlay Play;
		Play.Handle    = Handle;
		Play.Params    = Params;
		Play.Stream    = Move(Stream);
		Play.StopEpoch = StopEpoch.load(Atomic::MemoryOrderRelaxed);

		if (!PendingPlays.TryEnqueue(Play))
		{
			LOG_WARN_ONCE("Audio: the pending play queue is full; dropping a voice");
			ReleaseVoiceSlot(Handle.Index, Handle.Generation);
			DroppedVoices.fetch_add(1, Atomic::MemoryOrderRelaxed);
			return FAudioHandle::Invalid();
		}

		return Handle;
	}

	FAudioHandle FLuminaAudioContext::PlayAudioGraph(TSharedPtr<FAudioGraphInstance> Instance, const FAudioPlayParams& Params)
	{
		if (!Instance || !Instance->IsInitialized())
		{
			return FAudioHandle::Invalid();
		}

		FAudioHandle Handle;
		if (!AcquireVoiceSlot(Params.Priority, Handle))
		{
			return FAudioHandle::Invalid();
		}

		{
			FScopeLock Lock(GraphInstanceLock);
			GraphInstances[MakeGraphKey(Handle)] = Instance;
		}

		FPendingPlay Play;
		Play.Handle    = Handle;
		Play.Params    = Params;
		Play.Graph     = Move(Instance);
		Play.StopEpoch = StopEpoch.load(Atomic::MemoryOrderRelaxed);

		if (!PendingPlays.TryEnqueue(Play))
		{
			LOG_WARN_ONCE("Audio: the pending play queue is full; dropping a voice");
			ReleaseVoiceSlot(Handle.Index, Handle.Generation);
			DroppedVoices.fetch_add(1, Atomic::MemoryOrderRelaxed);
			return FAudioHandle::Invalid();
		}

		return Handle;
	}

	TSharedPtr<FAudioGraphInstance> FLuminaAudioContext::FindGraphInstance(FAudioHandle Handle) const
	{
		FScopeLock Lock(GraphInstanceLock);
		const auto It = GraphInstances.find(MakeGraphKey(Handle));
		return It != GraphInstances.end() ? It->second : nullptr;
	}

	void FLuminaAudioContext::ForgetGraphInstance(FAudioHandle Handle)
	{
		FScopeLock Lock(GraphInstanceLock);
		GraphInstances.erase(MakeGraphKey(Handle));
	}

	void FLuminaAudioContext::SweepGraphInstances()
	{
		FScopeLock Lock(GraphInstanceLock);

		for (auto It = GraphInstances.begin(); It != GraphInstances.end();)
		{
			const uint32 Slot = (uint32)(It->first & 0xFFFFFFFFull);
			const uint32 Generation = (uint32)(It->first >> 32);

			if (Slot >= MaxVoiceSlots || SlotGeneration[Slot].load(Atomic::MemoryOrderRelaxed) != Generation)
			{
				It = GraphInstances.erase(It);
			}
			else
			{
				++It;
			}
		}
	}

	bool FLuminaAudioContext::SetGraphFloatParameter(FAudioHandle Handle, const FName& Name, float Value)
	{
		const TSharedPtr<FAudioGraphInstance> Instance = FindGraphInstance(Handle);
		return Instance && Instance->SetFloatParameter(Name, Value);
	}

	bool FLuminaAudioContext::SetGraphIntParameter(FAudioHandle Handle, const FName& Name, int32 Value)
	{
		const TSharedPtr<FAudioGraphInstance> Instance = FindGraphInstance(Handle);
		return Instance && Instance->SetIntParameter(Name, Value);
	}

	bool FLuminaAudioContext::SetGraphBoolParameter(FAudioHandle Handle, const FName& Name, bool Value)
	{
		const TSharedPtr<FAudioGraphInstance> Instance = FindGraphInstance(Handle);
		return Instance && Instance->SetBoolParameter(Name, Value);
	}

	bool FLuminaAudioContext::TriggerGraphParameter(FAudioHandle Handle, const FName& Name)
	{
		const TSharedPtr<FAudioGraphInstance> Instance = FindGraphInstance(Handle);
		return Instance && Instance->TriggerParameter(Name);
	}

	float FLuminaAudioContext::GetGraphFloatOutput(FAudioHandle Handle, const FName& Name) const
	{
		const TSharedPtr<FAudioGraphInstance> Instance = FindGraphInstance(Handle);
		return Instance ? Instance->GetFloatOutput(Name) : 0.0f;
	}

	uint32 FLuminaAudioContext::GetGraphTriggerOutputCount(FAudioHandle Handle, const FName& Name) const
	{
		const TSharedPtr<FAudioGraphInstance> Instance = FindGraphInstance(Handle);
		return Instance ? Instance->GetTriggerOutputCount(Name) : 0;
	}

	void FLuminaAudioContext::StopSound(FAudioHandle Handle, EAudioStopMode Mode, float FadeSeconds)
	{
		if (Handle.IsValid())
		{
			PostToMixer(FAudioCommand::MakeStop(Handle, Mode, FadeSeconds));
		}
	}

	void FLuminaAudioContext::StopAllSounds(EAudioStopMode Mode, float FadeSeconds)
	{
		StopEpoch.fetch_add(1, Atomic::MemoryOrderRelaxed);
		PostToMixer(FAudioCommand::MakeStopAll(Mode, FadeSeconds));
	}

	void FLuminaAudioContext::SetVolume(FAudioHandle Handle, float Volume)
	{
		PostToMixer(FAudioCommand::MakeFloat(EAudioCommandType::SetVolume, Handle, Volume));
	}

	void FLuminaAudioContext::SetPitch(FAudioHandle Handle, float Pitch)
	{
		PostToMixer(FAudioCommand::MakeFloat(EAudioCommandType::SetPitch, Handle, Pitch));
	}

	void FLuminaAudioContext::SetLooping(FAudioHandle Handle, bool bLooping)
	{
		PostToMixer(FAudioCommand::MakeBool(EAudioCommandType::SetLooping, Handle, bLooping));
	}

	void FLuminaAudioContext::SetPosition(FAudioHandle Handle, FVector3 Position)
	{
		PostToMixer(FAudioCommand::MakeVector(EAudioCommandType::SetPosition, Handle, Position));
	}

	void FLuminaAudioContext::SetVelocity(FAudioHandle Handle, FVector3 Velocity)
	{
		PostToMixer(FAudioCommand::MakeVector(EAudioCommandType::SetVelocity, Handle, Velocity));
	}

	void FLuminaAudioContext::SetDirection(FAudioHandle Handle, FVector3 Direction)
	{
		PostToMixer(FAudioCommand::MakeVector(EAudioCommandType::SetDirection, Handle, Direction));
	}

	void FLuminaAudioContext::SetAttenuation(FAudioHandle Handle, const SAudioAttenuation& Attenuation)
	{
		PostToMixer(FAudioCommand::MakeAttenuation(Handle, Attenuation));
	}

	void FLuminaAudioContext::SetMinMaxDistance(FAudioHandle Handle, float MinDistance, float MaxDistance)
	{
		PostToMixer(FAudioCommand::MakeFloat2(EAudioCommandType::SetMinMaxDistance, Handle, MinDistance, MaxDistance));
	}

	void FLuminaAudioContext::SetPan(FAudioHandle Handle, float Pan)
	{
		PostToMixer(FAudioCommand::MakeFloat(EAudioCommandType::SetPan, Handle, Pan));
	}

	void FLuminaAudioContext::SetPaused(FAudioHandle Handle, bool bPaused)
	{
		PostToMixer(FAudioCommand::MakeBool(EAudioCommandType::SetPaused, Handle, bPaused));
	}

	void FLuminaAudioContext::SetBus(FAudioHandle Handle, EAudioBus Bus)
	{
		PostToMixer(FAudioCommand::MakeSetBus(Handle, Bus));
	}

	void FLuminaAudioContext::SetPriority(FAudioHandle Handle, uint8 Priority)
	{
		PostToMixer(FAudioCommand::MakeSetPriority(Handle, Priority));
	}

	void FLuminaAudioContext::SetOcclusion(FAudioHandle Handle, float Amount, float LowPassFrequency, float VolumeAttenuation)
	{
		FAudioCommand Cmd = FAudioCommand::Make(EAudioCommandType::SetOcclusion, Handle);
		Cmd.ValueA = Amount;
		Cmd.ValueB = LowPassFrequency;
		Cmd.ValueC = VolumeAttenuation;
		PostToMixer(Cmd);
	}

	void FLuminaAudioContext::SetLowPassCutoff(FAudioHandle Handle, float CutoffHz)
	{
		PostToMixer(FAudioCommand::MakeFloat(EAudioCommandType::SetLowPassCutoff, Handle, CutoffHz));
	}

	void FLuminaAudioContext::FadeTo(FAudioHandle Handle, float Volume, float Seconds)
	{
		PostToMixer(FAudioCommand::MakeFloat2(EAudioCommandType::FadeTo, Handle, Volume, Seconds));
	}

	void FLuminaAudioContext::SeekToFrame(FAudioHandle Handle, uint64 Frame)
	{
		PostToMixer(FAudioCommand::MakeSeekToFrame(Handle, Frame));
	}

	EAudioVoiceState FLuminaAudioContext::GetVoiceState(FAudioHandle Handle) const
	{
		if (!Handle.IsValid() || Handle.Index >= MaxVoiceSlots)
		{
			return EAudioVoiceState::Free;
		}

		if (SlotGeneration[Handle.Index].load(Atomic::MemoryOrderRelaxed) != Handle.Generation)
		{
			return EAudioVoiceState::Free;
		}

		return (EAudioVoiceState)SlotState[Handle.Index].load(Atomic::MemoryOrderAcquire);
	}

	uint64 FLuminaAudioContext::GetPlaybackFrame(FAudioHandle Handle) const
	{
		if (!Handle.IsValid() || Handle.Index >= MaxVoiceSlots)
		{
			return 0;
		}

		if (SlotGeneration[Handle.Index].load(Atomic::MemoryOrderRelaxed) != Handle.Generation)
		{
			return 0;
		}

		return Mixer.GetSlotFrame(Handle.Index);
	}

	void FLuminaAudioContext::UpdateListener(uint32 ListenerIndex, FVector3 Position, FQuat Rotation, FVector3 Velocity)
	{
		PostToMixer(FAudioCommand::MakeUpdateListener(ListenerIndex, Position, Rotation, Velocity));
	}

	void FLuminaAudioContext::SetListenerEnabled(uint32 ListenerIndex, bool bEnabled)
	{
		Mixer.SetListenerEnabled(ListenerIndex, bEnabled);
	}

	void FLuminaAudioContext::SetSuspended(bool bInSuspended)
	{
		if (bSuspended.exchange(bInSuspended, Atomic::MemoryOrderAcqRel) == bInSuspended)
		{
			return;
		}

		if (!Device)
		{
			return;
		}

		// Voices keep their state; only the endpoint stops, so playback resumes where it left off.
		if (bInSuspended)
		{
			Device->Stop();
		}
		else
		{
			Device->Start();
		}
	}

	void FLuminaAudioContext::SetMaxVoiceCount(uint32 MaxVoices)
	{
		const uint32 Clamped = Math::Clamp(MaxVoices, 1u, MaxVoiceSlots);
		MaxActiveVoices.store(Clamped, Atomic::MemoryOrderRelaxed);
		Mixer.SetMaxVoiceCount(Clamped);
	}

	void FLuminaAudioContext::ApplyDeviceConfig(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames)
	{
		PendingSampleRate.store(SampleRate, Atomic::MemoryOrderRelaxed);
		PendingChannels.store(Channels, Atomic::MemoryOrderRelaxed);
		PendingPeriodFrames.store(PeriodFrames, Atomic::MemoryOrderRelaxed);
		bDeviceConfigDirty.store(true, Atomic::MemoryOrderRelease);
	}

	FAudioDeviceInfo FLuminaAudioContext::GetDeviceInfo() const
	{
		FAudioDeviceInfo Info;
		Info.SampleRate    = Mixer.GetSampleRate();
		Info.Channels      = Mixer.GetChannelCount();
		Info.PeriodFrames  = Device ? Device->GetPeriodFrames() : 0;
		Info.ListenerCount = FAudioMixer::MaxListeners;
		return Info;
	}

	TSharedPtr<FProceduralAudioStream> FLuminaAudioContext::CreateProceduralStream(uint32 SampleRate, uint32 ChannelCount, uint32 BufferFrames)
	{
		return MakeShared<FProceduralAudioStream>(SampleRate, ChannelCount, BufferFrames);
	}
}
