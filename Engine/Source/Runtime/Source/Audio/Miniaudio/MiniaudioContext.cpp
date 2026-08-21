#include "RuntimePCH.h"
#include "MiniaudioContext.h"

#include "Core/Math/Math.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "MiniAudio/miniaudio.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

namespace Lumina
{
	namespace
	{
		constexpr uint32 MaxPlaysPerPump    = 32;
		constexpr uint32 MaxCommandsPerPump = 512;

		void* MiniAudioMalloc(size_t Size, void*)
		{
			LUMINA_MEMORY_SCOPE("Audio");
			return Memory::Malloc(Size);
		}

		void* MiniAudioRealloc(void* P, size_t Size, void*)
		{
			LUMINA_MEMORY_SCOPE("Audio");
			return Memory::Realloc(P, Size);
		}

		void MiniAudioFree(void* P, void*)
		{
			Memory::Free(P);
		}

		ma_attenuation_model ToMiniaudio(EAudioAttenuationModel Model)
		{
			switch (Model)
			{
			case EAudioAttenuationModel::None:        return ma_attenuation_model_none;
			case EAudioAttenuationModel::Linear:      return ma_attenuation_model_linear;
			case EAudioAttenuationModel::Exponential: return ma_attenuation_model_exponential;
			case EAudioAttenuationModel::Inverse:
			default:                                  return ma_attenuation_model_inverse;
			}
		}
	}

	FMiniaudioContext::FMiniaudioContext()
	{
		AllocationCallbacks.pUserData = nullptr;
		AllocationCallbacks.onMalloc  = MiniAudioMalloc;
		AllocationCallbacks.onRealloc = MiniAudioRealloc;
		AllocationCallbacks.onFree    = MiniAudioFree;

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			BusVolumes[i].store(1.0f, Atomic::MemoryOrderRelaxed);
			bBusMuted[i].store(false, Atomic::MemoryOrderRelaxed);
			BusReverbSends[i].store(0.0f, Atomic::MemoryOrderRelaxed);
		}

		for (uint32 i = 0; i < MaxVoiceSlots; ++i)
		{
			SlotState[i].store((uint8)EAudioVoiceState::Free, Atomic::MemoryOrderRelaxed);
			SlotGeneration[i].store(0, Atomic::MemoryOrderRelaxed);
			SlotPriority[i].store(0, Atomic::MemoryOrderRelaxed);
			SlotFrame[i].store(0, Atomic::MemoryOrderRelaxed);
			SlotCanceled[i].store(false, Atomic::MemoryOrderRelaxed);
			FreeSlots.enqueue(i);
		}

		PendingSampleRate.store(48000, Atomic::MemoryOrderRelaxed);
		PendingChannels.store(0, Atomic::MemoryOrderRelaxed);
		PendingPeriodFrames.store(0, Atomic::MemoryOrderRelaxed);

		if (!CreateEngine(48000, 0, 0))
		{
			return;
		}

		// PumpCounter is allocated lazily on the first Update(); Audio::Initialize runs before
		// Task::Initialize, so the Jobs system isn't up yet here.
		bRunning.store(true, Atomic::MemoryOrderRelease);
	}

	FMiniaudioContext::~FMiniaudioContext()
	{
		bRunning.store(false, Atomic::MemoryOrderRelease);

		// Let any in-flight pump job finish before we tear down its voices / the engine.
		if (PumpCounter != nullptr)
		{
			Jobs::WaitForCounter(PumpCounter, 0);
			Jobs::FreeCounter(PumpCounter);
			PumpCounter = nullptr;
		}

		DestroyEngine();
	}

	bool FMiniaudioContext::CreateEngine(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames)
	{
		ma_engine_config Config = ma_engine_config_init();
		Config.allocationCallbacks           = AllocationCallbacks;
		Config.listenerCount                 = MA_ENGINE_MAX_LISTENERS;
		Config.sampleRate                    = SampleRate;
		Config.channels                      = Channels;
		Config.periodSizeInFrames            = PeriodFrames;
		Config.gainSmoothTimeInMilliseconds  = 8;

		if (ma_engine_init(&Config, &Engine) != MA_SUCCESS)
		{
			LOG_ERROR("FMiniaudioContext: failed to initialize the miniaudio engine");
			return false;
		}

		bEngineInitialized.store(true, Atomic::MemoryOrderRelease);

		// Only listener 0 is live until a world enables more (split screen).
		for (uint32 i = 1; i < ma_engine_get_listener_count(&Engine); ++i)
		{
			ma_engine_listener_set_enabled(&Engine, i, MA_FALSE);
		}

		const float SmoothMs = 10.0f;
		VolumeSmoothFrames.store((uint32)(ma_engine_get_sample_rate(&Engine) * SmoothMs / 1000.0f), Atomic::MemoryOrderRelaxed);

		CreateBusGroups();
		return true;
	}

	void FMiniaudioContext::DestroyEngine()
	{
		// Cleared first: the game thread gates every direct bus/device touch on this.
		if (!bEngineInitialized.exchange(false, Atomic::MemoryOrderAcqRel))
		{
			return;
		}

		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			if (Voices[Slot])
			{
				UninitSound(*Voices[Slot]);
				Voices[Slot].reset();
			}
		}

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			if (bBusSplitterInitialized[i])
			{
				ma_splitter_node_uninit(&BusSplitters[i], &AllocationCallbacks);
				bBusSplitterInitialized[i] = false;
			}
		}

		Reverb.Uninit(&AllocationCallbacks);
		DestroyBusGroups();

		ma_engine_uninit(&Engine);

		ResetVoiceSlots();
	}

	void FMiniaudioContext::CreateBusGroups()
	{
		// Master attaches to the endpoint; every other bus feeds Master, so Master scales the whole mix.
		ma_sound_group_config MasterConfig = ma_sound_config_init_2(&Engine);
		MasterConfig.flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
		if (ma_sound_group_init_ex(&Engine, &MasterConfig, &BusGroups[(uint32)EAudioBus::Master]) != MA_SUCCESS)
		{
			LOG_ERROR("FMiniaudioContext: failed to create the Master bus");
			return;
		}
		bBusGroupInitialized[(uint32)EAudioBus::Master] = true;
		ma_sound_group_start(&BusGroups[(uint32)EAudioBus::Master]);

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			if (i == (uint32)EAudioBus::Master)
			{
				continue;
			}

			ma_sound_group_config Config = ma_sound_config_init_2(&Engine);
			Config.flags               = MA_SOUND_FLAG_NO_SPATIALIZATION;
			Config.pInitialAttachment  = &BusGroups[(uint32)EAudioBus::Master];

			if (ma_sound_group_init_ex(&Engine, &Config, &BusGroups[i]) != MA_SUCCESS)
			{
				LOG_ERROR("FMiniaudioContext: failed to create bus {}", ToString((EAudioBus)i));
				continue;
			}
			bBusGroupInitialized[i] = true;
			ma_sound_group_start(&BusGroups[i]);
		}

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			if (!bBusGroupInitialized[i])
			{
				continue;
			}
			const float Volume = bBusMuted[i].load(Atomic::MemoryOrderRelaxed) ? 0.0f : BusVolumes[i].load(Atomic::MemoryOrderRelaxed);
			ma_sound_group_set_volume(&BusGroups[i], Volume);
		}

		bReverbRoutingDirty.store(true, Atomic::MemoryOrderRelease);
	}

	void FMiniaudioContext::DestroyBusGroups()
	{
		// Children first so nothing is attached to Master when it goes away.
		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			if (i != (uint32)EAudioBus::Master && bBusGroupInitialized[i])
			{
				ma_sound_group_uninit(&BusGroups[i]);
				bBusGroupInitialized[i] = false;
			}
		}

		if (bBusGroupInitialized[(uint32)EAudioBus::Master])
		{
			ma_sound_group_uninit(&BusGroups[(uint32)EAudioBus::Master]);
			bBusGroupInitialized[(uint32)EAudioBus::Master] = false;
		}
	}

	void FMiniaudioContext::Update()
	{
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
			// Park-capable: the pump is long-lived and waits, so it must release its worker rather than hold one.
			Jobs::RunJob(&FMiniaudioContext::PumpEntry, this, Jobs::EJobPriority::Normal, PumpCounter, "Audio::Pump", /*bMayPark*/ true);
		}
	}

	void FMiniaudioContext::PumpEntry(void* Arg, uint32 /*WorkerIndex*/)
	{
		FMiniaudioContext* Self = static_cast<FMiniaudioContext*>(Arg);
		Self->PumpOnce();
		Self->bPumpActive.store(false, Atomic::MemoryOrderRelease);
	}

	void FMiniaudioContext::PumpOnce()
	{
		LUMINA_PROFILE_SECTION_COLORED("Audio::Pump", tracy::Color::Orange);

		ApplyPendingDeviceConfig();

		if (!bEngineInitialized.load(Atomic::MemoryOrderAcquire))
		{
			return;
		}

		if (bReverbRoutingDirty.exchange(false, Atomic::MemoryOrderAcqRel))
		{
			ApplyReverbRouting();
		}

		// Plays are drained before commands so a play followed by a tweak in the same frame lands on
		// the voice rather than being dropped for a handle the pump hasn't seen yet.
		FPendingPlay Play;
		uint32 PlaysProcessed = 0;
		while (PlaysProcessed < MaxPlaysPerPump && PendingPlays.try_dequeue(Play))
		{
			ProcessPendingPlay(Play);
			++PlaysProcessed;
		}

		FAudioCommand Cmd;
		uint32 CommandsProcessed = 0;
		while (CommandsProcessed < MaxCommandsPerPump && CommandQueue.try_dequeue(Cmd))
		{
			ProcessCommand(Cmd);
			++CommandsProcessed;
		}

		if (bDopplerDirty.exchange(false, Atomic::MemoryOrderAcqRel))
		{
			const float Scale = DopplerScale.load(Atomic::MemoryOrderRelaxed);
			for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
			{
				if (Voices[Slot] && Voices[Slot]->bInitialized)
				{
					ma_sound_set_doppler_factor(&Voices[Slot]->Sound, Voices[Slot]->DopplerFactor * Scale);
				}
			}
		}

		CleanupFinishedSounds();
		PublishVoiceState();
	}

	bool FMiniaudioContext::AcquireVoiceSlot(uint8 Priority, FAudioHandle& OutHandle)
	{
		uint32 Slot = 0;
		bool bTookFreeSlot = false;

		if (ActiveVoices.load(Atomic::MemoryOrderRelaxed) < MaxActiveVoices.load(Atomic::MemoryOrderRelaxed) && FreeSlots.try_dequeue(Slot))
		{
			bTookFreeSlot = true;
		}
		else
		{
			FScopeLock Lock(SlotLock);

			if (ActiveVoices.load(Atomic::MemoryOrderRelaxed) < MaxActiveVoices.load(Atomic::MemoryOrderRelaxed) && FreeSlots.try_dequeue(Slot))
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
		SlotFrame[Slot].store(0, Atomic::MemoryOrderRelaxed);
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

	void FMiniaudioContext::ReleaseVoiceSlot(uint32 Slot, uint32 Generation)
	{
		FScopeLock Lock(SlotLock);

		// A newer voice may already own this slot (priority takeover); leave its claim intact.
		if (SlotGeneration[Slot].load(Atomic::MemoryOrderRelaxed) != Generation)
		{
			return;
		}

		SlotState[Slot].store((uint8)EAudioVoiceState::Free, Atomic::MemoryOrderRelease);
		SlotPriority[Slot].store(0, Atomic::MemoryOrderRelaxed);
		ActiveVoices.fetch_sub(1, Atomic::MemoryOrderRelaxed);
		FreeSlots.enqueue(Slot);
	}

	void FMiniaudioContext::ResetVoiceSlots()
	{
		FScopeLock Lock(SlotLock);

		uint32 Discard = 0;
		while (FreeSlots.try_dequeue(Discard))
		{
		}

		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			SlotState[Slot].store((uint8)EAudioVoiceState::Free, Atomic::MemoryOrderRelaxed);
			SlotGeneration[Slot].store(0, Atomic::MemoryOrderRelaxed);
			SlotPriority[Slot].store(0, Atomic::MemoryOrderRelaxed);
			SlotFrame[Slot].store(0, Atomic::MemoryOrderRelaxed);
			SlotCanceled[Slot].store(false, Atomic::MemoryOrderRelaxed);
			FreeSlots.enqueue(Slot);
		}

		ActiveVoices.store(0, Atomic::MemoryOrderRelease);
	}

	FMiniaudioContext::FActiveSound* FMiniaudioContext::FindSound(FAudioHandle Handle)
	{
		if (!Handle.IsValid() || Handle.Index >= MaxVoiceSlots)
		{
			return nullptr;
		}

		FActiveSound* Sound = Voices[Handle.Index].get();
		return (Sound != nullptr && Sound->Handle == Handle) ? Sound : nullptr;
	}

	FAudioHandle FMiniaudioContext::PlayAudio(const TSharedPtr<FAudioData>& Data, const FAudioPlayParams& Params)
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

		PendingPlays.enqueue(std::move(Play));
		return Handle;
	}

	FAudioHandle FMiniaudioContext::PlayFile(FStringView File, const FAudioPlayParams& Params)
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

		PendingPlays.enqueue(std::move(Play));
		return Handle;
	}

	FAudioHandle FMiniaudioContext::PlayProceduralStream(TSharedPtr<FProceduralAudioStream> Stream, const FAudioPlayParams& Params)
	{
		if (!Stream || Stream->GetDataSource() == nullptr)
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
		Play.Stream    = std::move(Stream);
		Play.StopEpoch = StopEpoch.load(Atomic::MemoryOrderRelaxed);

		PendingPlays.enqueue(std::move(Play));
		return Handle;
	}

	void FMiniaudioContext::ProcessPendingPlay(FPendingPlay& Play)
	{
		const uint32 Slot = Play.Handle.Index;

		// The voice was stopped or flushed before the pump ever saw it.
		if (Play.StopEpoch != StopEpoch.load(Atomic::MemoryOrderRelaxed) || SlotCanceled[Slot].load(Atomic::MemoryOrderRelaxed))
		{
			ReleaseVoiceSlot(Slot, Play.Handle.Generation);
			return;
		}

		// Priority takeover: this slot's previous occupant is evicted here.
		if (Voices[Slot])
		{
			UninitSound(*Voices[Slot]);
			Voices[Slot].reset();
		}

		// Heap-allocated; ma_decoder/ma_sound store pointers in the node graph and must not move.
		TUniquePtr<FActiveSound> NewSound = MakeUnique<FActiveSound>();
		NewSound->Handle        = Play.Handle;
		NewSound->Priority      = Play.Params.Priority;
		NewSound->Bus           = Play.Params.Bus;
		NewSound->BaseVolume    = Play.Params.Volume;
		NewSound->Source        = Play.Data;
		NewSound->Procedural    = Play.Stream;
		NewSound->DopplerFactor = Play.Params.Attenuation.DopplerFactor;

		ma_data_source* DataSource = nullptr;

		if (NewSound->Procedural)
		{
			DataSource = NewSound->Procedural->GetDataSource();
		}
		else
		{
			const uint8* Bytes = nullptr;
			size_t ByteCount = 0;

			if (NewSound->Source)
			{
				Bytes     = NewSound->Source->Bytes.data();
				ByteCount = NewSound->Source->Bytes.size();
			}
			else
			{
				if (!VFS::ReadFile(NewSound->Bytes, FStringView(Play.Path)))
				{
					LOG_WARN("Audio: failed to read file '{}'", Play.Path.c_str());
					ReleaseVoiceSlot(Slot, Play.Handle.Generation);
					return;
				}
				Bytes     = NewSound->Bytes.data();
				ByteCount = NewSound->Bytes.size();
			}

			// Decode straight to the mix format so the mixer never converts per frame.
			ma_decoder_config DecoderConfig = ma_decoder_config_init(ma_format_f32, 0, ma_engine_get_sample_rate(&Engine));
			if (ma_decoder_init_memory(Bytes, ByteCount, &DecoderConfig, &NewSound->Decoder) != MA_SUCCESS)
			{
				LOG_WARN("Audio: failed to decode audio data");
				ReleaseVoiceSlot(Slot, Play.Handle.Generation);
				return;
			}

			NewSound->bDecoderInitialized = true;
			DataSource = &NewSound->Decoder;
		}

		ma_sound_config SoundConfig = ma_sound_config_init_2(&Engine);
		SoundConfig.pDataSource                 = DataSource;
		SoundConfig.volumeSmoothTimeInPCMFrames = VolumeSmoothFrames.load(Atomic::MemoryOrderRelaxed);
		SoundConfig.initialSeekPointInPCMFrames = Play.Params.StartFrame;
		SoundConfig.pInitialAttachment          = GetBusGroup(NewSound->Bus);

		if (ma_sound_init_ex(&Engine, &SoundConfig, &NewSound->Sound) != MA_SUCCESS)
		{
			LOG_WARN("Audio: failed to initialize voice");
			if (NewSound->bDecoderInitialized)
			{
				ma_decoder_uninit(&NewSound->Decoder);
			}
			ReleaseVoiceSlot(Slot, Play.Handle.Generation);
			return;
		}

		NewSound->bInitialized = true;

		ma_sound_set_pitch(&NewSound->Sound, Play.Params.Pitch);
		ma_sound_set_looping(&NewSound->Sound, (Play.Params.bLooping && !NewSound->Procedural) ? MA_TRUE : MA_FALSE);
		ma_sound_set_spatialization_enabled(&NewSound->Sound, Play.Params.bSpatialized ? MA_TRUE : MA_FALSE);

		if (Play.Params.bSpatialized)
		{
			ma_sound_set_position(&NewSound->Sound, Play.Params.Position.x, Play.Params.Position.y, Play.Params.Position.z);
			ma_sound_set_velocity(&NewSound->Sound, Play.Params.Velocity.x, Play.Params.Velocity.y, Play.Params.Velocity.z);
			ma_sound_set_direction(&NewSound->Sound, Play.Params.Direction.x, Play.Params.Direction.y, Play.Params.Direction.z);
		}

		ApplyVoiceAttenuation(*NewSound, Play.Params.Attenuation);
		ApplyVoiceVolume(*NewSound);

		if (Play.Params.bUseOcclusion)
		{
			// Build the filter up front, wide open, so the first trace result doesn't rewire mid-playback.
			ApplyVoiceLowPass(*NewSound, (float)ma_engine_get_sample_rate(&Engine) * 0.49f);
		}

		if (Play.Params.FadeInSeconds > 0.0f)
		{
			ma_sound_set_fade_in_milliseconds(&NewSound->Sound, 0.0f, 1.0f, (ma_uint64)(Play.Params.FadeInSeconds * 1000.0f));
		}

		if (Play.Params.StartDelaySeconds > 0.0f)
		{
			const ma_uint64 Now = ma_engine_get_time_in_milliseconds(&Engine);
			ma_sound_set_start_time_in_milliseconds(&NewSound->Sound, Now + (ma_uint64)(Play.Params.StartDelaySeconds * 1000.0f));
		}

		if (Play.Params.bStartPaused)
		{
			NewSound->bPaused = true;
			SlotState[Slot].store((uint8)EAudioVoiceState::Paused, Atomic::MemoryOrderRelease);
		}
		else
		{
			ma_sound_start(&NewSound->Sound);
		}

		Voices[Slot] = std::move(NewSound);
	}

	void FMiniaudioContext::ProcessCommand(const FAudioCommand& Cmd)
	{
		if (Cmd.Type == EAudioCommandType::StopAll)
		{
			for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
			{
				if (!Voices[Slot])
				{
					continue;
				}

				FActiveSound& Sound = *Voices[Slot];
				if (Cmd.StopMode == EAudioStopMode::AllowFadeOut && Sound.bInitialized && !Sound.bPaused)
				{
					ma_sound_stop_with_fade_in_milliseconds(&Sound.Sound, (ma_uint64)(Cmd.ValueA * 1000.0f));
					Sound.bStopping = true;
				}
				else
				{
					const uint32 Generation = Sound.Handle.Generation;
					UninitSound(Sound);
					Voices[Slot].reset();
					ReleaseVoiceSlot(Slot, Generation);
				}
			}
			return;
		}

		if (Cmd.Type == EAudioCommandType::UpdateListener)
		{
			const uint32 ListenerIndex = Cmd.ByteValue;
			if (ListenerIndex >= ma_engine_get_listener_count(&Engine))
			{
				return;
			}

			const FVector3 Forward = Math::Normalize(Math::Rotate(Cmd.Rotation, FVector3(0.0f, 0.0f, 1.0f)));
			const FVector3 Up      = Math::Normalize(Math::Rotate(Cmd.Rotation, FVector3(0.0f, 1.0f, 0.0f)));

			ma_engine_listener_set_position(&Engine, ListenerIndex, Cmd.Vector.x, Cmd.Vector.y, Cmd.Vector.z);
			ma_engine_listener_set_direction(&Engine, ListenerIndex, Forward.x, Forward.y, Forward.z);
			ma_engine_listener_set_world_up(&Engine, ListenerIndex, Up.x, Up.y, Up.z);
			ma_engine_listener_set_velocity(&Engine, ListenerIndex, Cmd.VectorB.x, Cmd.VectorB.y, Cmd.VectorB.z);
			return;
		}

		FActiveSound* Sound = FindSound(Cmd.Handle);
		if (Sound == nullptr)
		{
			return;
		}

		switch (Cmd.Type)
		{
		case EAudioCommandType::StopSound:
		{
			if (Cmd.StopMode == EAudioStopMode::AllowFadeOut && !Sound->bPaused)
			{
				ma_sound_stop_with_fade_in_milliseconds(&Sound->Sound, (ma_uint64)(Cmd.ValueA * 1000.0f));
				Sound->bStopping = true;
			}
			else
			{
				const uint32 Slot = Cmd.Handle.Index;
				const uint32 Generation = Sound->Handle.Generation;
				UninitSound(*Sound);
				Voices[Slot].reset();
				ReleaseVoiceSlot(Slot, Generation);
			}
			break;
		}

		case EAudioCommandType::SetVolume:
			Sound->BaseVolume = Cmd.ValueA;
			ApplyVoiceVolume(*Sound);
			break;

		case EAudioCommandType::SetPitch:
			ma_sound_set_pitch(&Sound->Sound, Cmd.ValueA);
			break;

		case EAudioCommandType::SetLooping:
			if (!Sound->Procedural)
			{
				ma_sound_set_looping(&Sound->Sound, Cmd.bValue ? MA_TRUE : MA_FALSE);
			}
			break;

		case EAudioCommandType::SetPosition:
			ma_sound_set_position(&Sound->Sound, Cmd.Vector.x, Cmd.Vector.y, Cmd.Vector.z);
			break;

		case EAudioCommandType::SetVelocity:
			ma_sound_set_velocity(&Sound->Sound, Cmd.Vector.x, Cmd.Vector.y, Cmd.Vector.z);
			break;

		case EAudioCommandType::SetDirection:
			ma_sound_set_direction(&Sound->Sound, Cmd.Vector.x, Cmd.Vector.y, Cmd.Vector.z);
			break;

		case EAudioCommandType::SetAttenuation:
			ApplyVoiceAttenuation(*Sound, Cmd.Attenuation);
			break;

		case EAudioCommandType::SetMinMaxDistance:
			ma_sound_set_min_distance(&Sound->Sound, Cmd.ValueA);
			ma_sound_set_max_distance(&Sound->Sound, Cmd.ValueB);
			break;

		case EAudioCommandType::SetPan:
			ma_sound_set_pan(&Sound->Sound, Cmd.ValueA);
			break;

		case EAudioCommandType::SetPaused:
		{
			if (Cmd.bValue && !Sound->bPaused)
			{
				// ma_sound_stop leaves the cursor where it is, so start() resumes rather than restarts.
				ma_sound_stop(&Sound->Sound);
				Sound->bPaused = true;
			}
			else if (!Cmd.bValue && Sound->bPaused)
			{
				ma_sound_start(&Sound->Sound);
				Sound->bPaused = false;
			}
			break;
		}

		case EAudioCommandType::SetBus:
			Sound->Bus = Cmd.Bus;
			AttachVoiceOutput(*Sound);
			break;

		case EAudioCommandType::SetPriority:
			Sound->Priority = Cmd.ByteValue;
			SlotPriority[Cmd.Handle.Index].store(Cmd.ByteValue, Atomic::MemoryOrderRelaxed);
			break;

		case EAudioCommandType::SetOcclusion:
		{
			const float Amount            = Math::Clamp(Cmd.ValueA, 0.0f, 1.0f);
			const float LowPassFrequency  = Cmd.ValueB;
			const float VolumeAttenuation = Cmd.ValueC;

			Sound->OcclusionGain = Math::Lerp(1.0f, Math::Clamp(VolumeAttenuation, 0.0f, 1.0f), Amount);
			ApplyVoiceVolume(*Sound);

			const float Nyquist = (float)ma_engine_get_sample_rate(&Engine) * 0.49f;
			const float Cutoff  = Math::Lerp(Nyquist, Math::Clamp(LowPassFrequency, 20.0f, Nyquist), Amount);
			ApplyVoiceLowPass(*Sound, Amount > 0.0001f ? Cutoff : 0.0f);
			break;
		}

		case EAudioCommandType::SetLowPassCutoff:
			ApplyVoiceLowPass(*Sound, Cmd.ValueA);
			break;

		case EAudioCommandType::FadeTo:
			ma_sound_set_fade_in_milliseconds(&Sound->Sound, -1.0f, Cmd.ValueA, (ma_uint64)(Cmd.ValueB * 1000.0f));
			break;

		case EAudioCommandType::SeekToFrame:
			if (!Sound->Procedural)
			{
				ma_sound_seek_to_pcm_frame(&Sound->Sound, Cmd.FrameValue);
			}
			break;

		default:
			break;
		}
	}

	void FMiniaudioContext::ApplyVoiceVolume(FActiveSound& Sound)
	{
		ma_sound_set_volume(&Sound.Sound, Sound.BaseVolume * Sound.OcclusionGain);
	}

	void FMiniaudioContext::ApplyVoiceAttenuation(FActiveSound& Sound, const SAudioAttenuation& Attenuation)
	{
		ma_sound_set_attenuation_model(&Sound.Sound, ToMiniaudio(Attenuation.Model));
		ma_sound_set_min_distance(&Sound.Sound, Attenuation.MinDistance);
		ma_sound_set_max_distance(&Sound.Sound, Attenuation.MaxDistance);
		ma_sound_set_rolloff(&Sound.Sound, Attenuation.Rolloff);
		ma_sound_set_min_gain(&Sound.Sound, Attenuation.MinGain);
		ma_sound_set_max_gain(&Sound.Sound, Attenuation.MaxGain);
		ma_sound_set_cone(&Sound.Sound,
			Math::Radians(Attenuation.ConeInnerAngle),
			Math::Radians(Attenuation.ConeOuterAngle),
			Attenuation.ConeOuterGain);
		ma_sound_set_directional_attenuation_factor(&Sound.Sound, Attenuation.DirectionalFactor);
		ma_sound_set_pan(&Sound.Sound, Attenuation.Pan);
		ma_sound_set_positioning(&Sound.Sound,
			Attenuation.Positioning == EAudioPositioning::Relative ? ma_positioning_relative : ma_positioning_absolute);

		Sound.DopplerFactor = Attenuation.DopplerFactor;
		ma_sound_set_doppler_factor(&Sound.Sound, Attenuation.DopplerFactor * DopplerScale.load(Atomic::MemoryOrderRelaxed));
	}

	void FMiniaudioContext::ApplyVoiceLowPass(FActiveSound& Sound, float CutoffHz)
	{
		const uint32 Channels   = ma_engine_get_channels(&Engine);
		const uint32 SampleRate = ma_engine_get_sample_rate(&Engine);
		const float Nyquist     = (float)SampleRate * 0.49f;
		const float Cutoff      = (CutoffHz <= 0.0f) ? Nyquist : Math::Clamp(CutoffHz, 20.0f, Nyquist);

		if (!Sound.bLowPassInitialized)
		{
			// A bypassed filter isn't worth a node; wait until something actually asks for muffling.
			if (CutoffHz <= 0.0f)
			{
				Sound.LowPassCutoff = 0.0f;
				return;
			}

			ma_lpf_node_config Config = ma_lpf_node_config_init(Channels, SampleRate, Cutoff, 2);
			if (ma_lpf_node_init(ma_engine_get_node_graph(&Engine), &Config, &AllocationCallbacks, &Sound.LowPass) != MA_SUCCESS)
			{
				LOG_WARN("Audio: failed to create the occlusion filter for a voice");
				return;
			}

			Sound.bLowPassInitialized = true;
			Sound.LowPassCutoff = Cutoff;
			AttachVoiceOutput(Sound);
			return;
		}

		if (Math::Abs(Sound.LowPassCutoff - Cutoff) < 1.0f)
		{
			return;
		}

		ma_lpf_config Config = ma_lpf_config_init(ma_format_f32, Channels, SampleRate, Cutoff, 2);
		ma_lpf_node_reinit(&Config, &Sound.LowPass);
		Sound.LowPassCutoff = Cutoff;
	}

	bool FMiniaudioContext::AttachVoiceOutput(FActiveSound& Sound)
	{
		if (!Sound.bInitialized)
		{
			return false;
		}

		ma_node* Target = (ma_node*)GetBusGroup(Sound.Bus);

		if (Sound.bLowPassInitialized)
		{
			// Attaching to a new node detaches from the old one, so ordering here is safe while running.
			ma_node_attach_output_bus(&Sound.LowPass, 0, Target, 0);
			return ma_node_attach_output_bus(&Sound.Sound, 0, &Sound.LowPass, 0) == MA_SUCCESS;
		}

		return ma_node_attach_output_bus(&Sound.Sound, 0, Target, 0) == MA_SUCCESS;
	}

	void FMiniaudioContext::CleanupFinishedSounds()
	{
		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			if (!Voices[Slot])
			{
				continue;
			}

			FActiveSound& Sound = *Voices[Slot];
			if (!Sound.bInitialized)
			{
				continue;
			}

			bool bFinished = false;
			if (Sound.bStopping)
			{
				bFinished = !ma_sound_is_playing(&Sound.Sound);
			}
			else if (!Sound.Procedural && !Sound.bPaused)
			{
				// Procedural voices stay alive even when their ring buffer momentarily runs dry.
				bFinished = ma_sound_at_end(&Sound.Sound) == MA_TRUE;
			}

			if (bFinished)
			{
				const uint32 Generation = Sound.Handle.Generation;
				UninitSound(Sound);
				Voices[Slot].reset();
				ReleaseVoiceSlot(Slot, Generation);
			}
		}
	}

	void FMiniaudioContext::PublishVoiceState()
	{
		for (uint32 Slot = 0; Slot < MaxVoiceSlots; ++Slot)
		{
			if (!Voices[Slot] || !Voices[Slot]->bInitialized)
			{
				continue;
			}

			const FActiveSound& Sound = *Voices[Slot];

			// The data source cursor, not the node's local time, so seeks and loop wraps are reflected.
			ma_uint64 Cursor = 0;
			if (ma_sound_get_cursor_in_pcm_frames(&Sound.Sound, &Cursor) != MA_SUCCESS)
			{
				Cursor = ma_sound_get_time_in_pcm_frames(&Sound.Sound);
			}

			SlotFrame[Slot].store(Cursor, Atomic::MemoryOrderRelaxed);
			SlotState[Slot].store(Sound.bPaused ? (uint8)EAudioVoiceState::Paused : (uint8)EAudioVoiceState::Playing, Atomic::MemoryOrderRelease);
		}
	}

	void FMiniaudioContext::UninitSound(FActiveSound& Sound)
	{
		if (Sound.bInitialized)
		{
			ma_sound_uninit(&Sound.Sound);
			Sound.bInitialized = false;
		}

		if (Sound.bLowPassInitialized)
		{
			ma_lpf_node_uninit(&Sound.LowPass, &AllocationCallbacks);
			Sound.bLowPassInitialized = false;
		}

		if (Sound.bDecoderInitialized)
		{
			ma_decoder_uninit(&Sound.Decoder);
			Sound.bDecoderInitialized = false;
		}
	}

	void FMiniaudioContext::StopSound(FAudioHandle Handle, EAudioStopMode Mode, float FadeSeconds)
	{
		if (Handle.IsValid() && Handle.Index < MaxVoiceSlots &&
			SlotGeneration[Handle.Index].load(Atomic::MemoryOrderRelaxed) == Handle.Generation)
		{
			// Catches a stop issued before the pump has started the voice.
			SlotCanceled[Handle.Index].store(true, Atomic::MemoryOrderRelaxed);
		}
		CommandQueue.enqueue(FAudioCommand::MakeStop(Handle, Mode, FadeSeconds));
	}

	void FMiniaudioContext::StopAllSounds(EAudioStopMode Mode, float FadeSeconds)
	{
		StopEpoch.fetch_add(1, Atomic::MemoryOrderRelaxed);
		CommandQueue.enqueue(FAudioCommand::MakeStopAll(Mode, FadeSeconds));
	}

	void FMiniaudioContext::SetVolume(FAudioHandle Handle, float Volume)
	{
		CommandQueue.enqueue(FAudioCommand::MakeFloat(EAudioCommandType::SetVolume, Handle, Volume));
	}

	void FMiniaudioContext::SetPitch(FAudioHandle Handle, float Pitch)
	{
		CommandQueue.enqueue(FAudioCommand::MakeFloat(EAudioCommandType::SetPitch, Handle, Pitch));
	}

	void FMiniaudioContext::SetLooping(FAudioHandle Handle, bool bLooping)
	{
		CommandQueue.enqueue(FAudioCommand::MakeBool(EAudioCommandType::SetLooping, Handle, bLooping));
	}

	void FMiniaudioContext::SetPosition(FAudioHandle Handle, FVector3 Position)
	{
		CommandQueue.enqueue(FAudioCommand::MakeVector(EAudioCommandType::SetPosition, Handle, Position));
	}

	void FMiniaudioContext::SetVelocity(FAudioHandle Handle, FVector3 Velocity)
	{
		CommandQueue.enqueue(FAudioCommand::MakeVector(EAudioCommandType::SetVelocity, Handle, Velocity));
	}

	void FMiniaudioContext::SetDirection(FAudioHandle Handle, FVector3 Direction)
	{
		CommandQueue.enqueue(FAudioCommand::MakeVector(EAudioCommandType::SetDirection, Handle, Direction));
	}

	void FMiniaudioContext::SetAttenuation(FAudioHandle Handle, const SAudioAttenuation& Attenuation)
	{
		CommandQueue.enqueue(FAudioCommand::MakeAttenuation(Handle, Attenuation));
	}

	void FMiniaudioContext::SetMinMaxDistance(FAudioHandle Handle, float MinDistance, float MaxDistance)
	{
		CommandQueue.enqueue(FAudioCommand::MakeFloat2(EAudioCommandType::SetMinMaxDistance, Handle, MinDistance, MaxDistance));
	}

	void FMiniaudioContext::SetPan(FAudioHandle Handle, float Pan)
	{
		CommandQueue.enqueue(FAudioCommand::MakeFloat(EAudioCommandType::SetPan, Handle, Pan));
	}

	void FMiniaudioContext::SetPaused(FAudioHandle Handle, bool bPaused)
	{
		CommandQueue.enqueue(FAudioCommand::MakeBool(EAudioCommandType::SetPaused, Handle, bPaused));
	}

	void FMiniaudioContext::SetBus(FAudioHandle Handle, EAudioBus Bus)
	{
		CommandQueue.enqueue(FAudioCommand::MakeSetBus(Handle, Bus));
	}

	void FMiniaudioContext::SetPriority(FAudioHandle Handle, uint8 Priority)
	{
		CommandQueue.enqueue(FAudioCommand::MakeSetPriority(Handle, Priority));
	}

	void FMiniaudioContext::SetOcclusion(FAudioHandle Handle, float Amount, float LowPassFrequency, float VolumeAttenuation)
	{
		FAudioCommand Cmd = FAudioCommand::Make(EAudioCommandType::SetOcclusion, Handle);
		Cmd.ValueA = Amount;
		Cmd.ValueB = LowPassFrequency;
		Cmd.ValueC = VolumeAttenuation;
		CommandQueue.enqueue(Cmd);
	}

	void FMiniaudioContext::SetLowPassCutoff(FAudioHandle Handle, float CutoffHz)
	{
		CommandQueue.enqueue(FAudioCommand::MakeFloat(EAudioCommandType::SetLowPassCutoff, Handle, CutoffHz));
	}

	void FMiniaudioContext::FadeTo(FAudioHandle Handle, float Volume, float Seconds)
	{
		CommandQueue.enqueue(FAudioCommand::MakeFloat2(EAudioCommandType::FadeTo, Handle, Volume, Seconds));
	}

	void FMiniaudioContext::SeekToFrame(FAudioHandle Handle, uint64 Frame)
	{
		CommandQueue.enqueue(FAudioCommand::MakeSeekToFrame(Handle, Frame));
	}

	EAudioVoiceState FMiniaudioContext::GetVoiceState(FAudioHandle Handle) const
	{
		if (!Handle.IsValid() || Handle.Index >= MaxVoiceSlots)
		{
			return EAudioVoiceState::Free;
		}
		if (SlotGeneration[Handle.Index].load(Atomic::MemoryOrderAcquire) != Handle.Generation)
		{
			return EAudioVoiceState::Free;
		}
		return (EAudioVoiceState)SlotState[Handle.Index].load(Atomic::MemoryOrderAcquire);
	}

	uint64 FMiniaudioContext::GetPlaybackFrame(FAudioHandle Handle) const
	{
		if (!Handle.IsValid() || Handle.Index >= MaxVoiceSlots)
		{
			return 0;
		}
		if (SlotGeneration[Handle.Index].load(Atomic::MemoryOrderAcquire) != Handle.Generation)
		{
			return 0;
		}
		return SlotFrame[Handle.Index].load(Atomic::MemoryOrderRelaxed);
	}

	void FMiniaudioContext::UpdateListener(uint32 ListenerIndex, FVector3 Position, FQuat Rotation, FVector3 Velocity)
	{
		CommandQueue.enqueue(FAudioCommand::MakeUpdateListener(ListenerIndex, Position, Rotation, Velocity));
	}

	void FMiniaudioContext::SetListenerEnabled(uint32 ListenerIndex, bool bEnabled)
	{
		if (bEngineInitialized.load(Atomic::MemoryOrderAcquire) && ListenerIndex < ma_engine_get_listener_count(&Engine))
		{
			ma_engine_listener_set_enabled(&Engine, ListenerIndex, bEnabled ? MA_TRUE : MA_FALSE);
		}
	}

	uint32 FMiniaudioContext::GetListenerCount() const
	{
		return bEngineInitialized.load(Atomic::MemoryOrderAcquire) ? ma_engine_get_listener_count(&Engine) : 0;
	}

	void FMiniaudioContext::SetBusVolume(EAudioBus Bus, float Volume)
	{
		const uint32 Index = (uint32)Bus;
		BusVolumes[Index].store(Math::Max(Volume, 0.0f), Atomic::MemoryOrderRelaxed);

		if (bEngineInitialized.load(Atomic::MemoryOrderAcquire) && !bBusMuted[Index].load(Atomic::MemoryOrderRelaxed))
		{
			ma_sound_group_set_volume(&BusGroups[Index], Math::Max(Volume, 0.0f));
		}
	}

	float FMiniaudioContext::GetBusVolume(EAudioBus Bus) const
	{
		return BusVolumes[(uint32)Bus].load(Atomic::MemoryOrderRelaxed);
	}

	void FMiniaudioContext::SetBusMuted(EAudioBus Bus, bool bMuted)
	{
		const uint32 Index = (uint32)Bus;
		bBusMuted[Index].store(bMuted, Atomic::MemoryOrderRelaxed);

		if (bEngineInitialized.load(Atomic::MemoryOrderAcquire))
		{
			ma_sound_group_set_volume(&BusGroups[Index], bMuted ? 0.0f : BusVolumes[Index].load(Atomic::MemoryOrderRelaxed));
		}
	}

	bool FMiniaudioContext::IsBusMuted(EAudioBus Bus) const
	{
		return bBusMuted[(uint32)Bus].load(Atomic::MemoryOrderRelaxed);
	}

	void FMiniaudioContext::SetBusPitch(EAudioBus Bus, float Pitch)
	{
		if (bEngineInitialized.load(Atomic::MemoryOrderAcquire))
		{
			ma_sound_group_set_pitch(&BusGroups[(uint32)Bus], Math::Max(Pitch, 0.01f));
		}
	}

	void FMiniaudioContext::SetBusReverbSend(EAudioBus Bus, float SendLevel)
	{
		BusReverbSends[(uint32)Bus].store(Math::Clamp(SendLevel, 0.0f, 1.0f), Atomic::MemoryOrderRelaxed);
		bReverbRoutingDirty.store(true, Atomic::MemoryOrderRelease);
	}

	float FMiniaudioContext::GetBusReverbSend(EAudioBus Bus) const
	{
		return BusReverbSends[(uint32)Bus].load(Atomic::MemoryOrderRelaxed);
	}

	void FMiniaudioContext::SetReverbParams(const FAudioReverbParams& Params)
	{
		PendingReverbParams = Params;
		if (Reverb.IsInitialized())
		{
			Reverb.SetParams(Params);
		}
	}

	FAudioReverbParams FMiniaudioContext::GetReverbParams() const
	{
		return Reverb.IsInitialized() ? Reverb.GetParams() : PendingReverbParams;
	}

	void FMiniaudioContext::ApplyReverbRouting()
	{
		bool bAnySend = false;
		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			if (i != (uint32)EAudioBus::Master && BusReverbSends[i].load(Atomic::MemoryOrderRelaxed) > 0.0f)
			{
				bAnySend = true;
				break;
			}
		}

		if (!bAnySend)
		{
			if (Reverb.IsInitialized())
			{
				ma_node_set_state(Reverb.GetNode(), ma_node_state_stopped);
				for (uint32 i = 0; i < NumAudioBuses; ++i)
				{
					if (bBusSplitterInitialized[i])
					{
						ma_node_set_output_bus_volume(&BusSplitters[i], 1, 0.0f);
					}
				}
			}
			return;
		}

		const uint32 Channels   = ma_engine_get_channels(&Engine);
		const uint32 SampleRate = ma_engine_get_sample_rate(&Engine);

		if (!Reverb.IsInitialized())
		{
			if (Reverb.Init(ma_engine_get_node_graph(&Engine), Channels, SampleRate, &AllocationCallbacks) != MA_SUCCESS)
			{
				LOG_ERROR("FMiniaudioContext: failed to create the reverb node");
				return;
			}
			Reverb.SetParams(PendingReverbParams);
			ma_node_attach_output_bus(Reverb.GetNode(), 0, &BusGroups[(uint32)EAudioBus::Master], 0);
		}

		ma_node_set_state(Reverb.GetNode(), ma_node_state_started);

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			if (i == (uint32)EAudioBus::Master)
			{
				continue;
			}

			const float Send = BusReverbSends[i].load(Atomic::MemoryOrderRelaxed);

			if (!bBusSplitterInitialized[i])
			{
				if (Send <= 0.0f)
				{
					continue;
				}

				ma_splitter_node_config Config = ma_splitter_node_config_init(Channels);
				if (ma_splitter_node_init(ma_engine_get_node_graph(&Engine), &Config, &AllocationCallbacks, &BusSplitters[i]) != MA_SUCCESS)
				{
					LOG_WARN("FMiniaudioContext: failed to create the reverb send for bus {}", ToString((EAudioBus)i));
					continue;
				}

				bBusSplitterInitialized[i] = true;

				// Bus -> splitter, dry branch back to Master, wet branch into the reverb return.
				ma_node_attach_output_bus(&BusSplitters[i], 0, &BusGroups[(uint32)EAudioBus::Master], 0);
				ma_node_attach_output_bus(&BusSplitters[i], 1, Reverb.GetNode(), 0);
				ma_node_attach_output_bus(&BusGroups[i], 0, &BusSplitters[i], 0);
			}

			ma_node_set_output_bus_volume(&BusSplitters[i], 1, Send);
		}
	}

	void FMiniaudioContext::SetDopplerScale(float Scale)
	{
		DopplerScale.store(Math::Max(Scale, 0.0f), Atomic::MemoryOrderRelaxed);
		bDopplerDirty.store(true, Atomic::MemoryOrderRelease);
	}

	void FMiniaudioContext::SetSuspended(bool bInSuspended)
	{
		if (bSuspended.exchange(bInSuspended, Atomic::MemoryOrderAcqRel) == bInSuspended || !bEngineInitialized.load(Atomic::MemoryOrderAcquire))
		{
			return;
		}

		ma_device* Device = ma_engine_get_device(&Engine);
		if (Device == nullptr)
		{
			return;
		}

		if (bInSuspended)
		{
			ma_device_stop(Device);
		}
		else
		{
			ma_device_start(Device);
		}
	}

	void FMiniaudioContext::SetMaxVoiceCount(uint32 MaxVoices)
	{
		MaxActiveVoices.store(Math::Clamp(MaxVoices, 8u, MaxVoiceSlots), Atomic::MemoryOrderRelaxed);
	}

	void FMiniaudioContext::SetVolumeSmoothing(float Milliseconds)
	{
		if (!bEngineInitialized.load(Atomic::MemoryOrderAcquire))
		{
			return;
		}
		const float Clamped = Math::Clamp(Milliseconds, 0.0f, 200.0f);
		VolumeSmoothFrames.store((uint32)(ma_engine_get_sample_rate(&Engine) * Clamped / 1000.0f), Atomic::MemoryOrderRelaxed);
	}

	void FMiniaudioContext::ApplyDeviceConfig(uint32 SampleRate, uint32 Channels, uint32 PeriodFrames)
	{
		// Compared against the last requested config, not the resolved device, so "0 = native" stays stable.
		if (PendingSampleRate.load(Atomic::MemoryOrderRelaxed) == SampleRate &&
			PendingChannels.load(Atomic::MemoryOrderRelaxed) == Channels &&
			PendingPeriodFrames.load(Atomic::MemoryOrderRelaxed) == PeriodFrames)
		{
			return;
		}

		PendingSampleRate.store(SampleRate, Atomic::MemoryOrderRelaxed);
		PendingChannels.store(Channels, Atomic::MemoryOrderRelaxed);
		PendingPeriodFrames.store(PeriodFrames, Atomic::MemoryOrderRelaxed);
		bDeviceConfigDirty.store(true, Atomic::MemoryOrderRelease);
	}

	void FMiniaudioContext::ApplyPendingDeviceConfig()
	{
		if (!bDeviceConfigDirty.exchange(false, Atomic::MemoryOrderAcqRel))
		{
			return;
		}

		// Every voice dies with the old device; handles held by callers go stale.
		DestroyEngine();

		if (!CreateEngine(PendingSampleRate.load(Atomic::MemoryOrderRelaxed),
			PendingChannels.load(Atomic::MemoryOrderRelaxed),
			PendingPeriodFrames.load(Atomic::MemoryOrderRelaxed)))
		{
			LOG_ERROR("FMiniaudioContext: device rebuild failed, audio is now silent");
			return;
		}

		if (bSuspended.load(Atomic::MemoryOrderRelaxed))
		{
			if (ma_device* Device = ma_engine_get_device(&Engine))
			{
				ma_device_stop(Device);
			}
		}
	}

	FAudioDeviceInfo FMiniaudioContext::GetDeviceInfo() const
	{
		FAudioDeviceInfo Info;
		if (!bEngineInitialized.load(Atomic::MemoryOrderAcquire))
		{
			return Info;
		}

		ma_engine* MutableEngine = const_cast<ma_engine*>(&Engine);
		Info.SampleRate    = ma_engine_get_sample_rate(&Engine);
		Info.Channels      = ma_engine_get_channels(&Engine);
		Info.ListenerCount = ma_engine_get_listener_count(&Engine);

		if (const ma_device* Device = ma_engine_get_device(MutableEngine))
		{
			Info.PeriodFrames = Device->playback.internalPeriodSizeInFrames;
		}
		return Info;
	}

	TSharedPtr<FProceduralAudioStream> FMiniaudioContext::CreateProceduralStream(uint32 SampleRate, uint32 ChannelCount, uint32 BufferFrames)
	{
		return MakeShared<FProceduralAudioStream>(SampleRate, ChannelCount, BufferFrames);
	}
}
