#include "RuntimePCH.h"
#include "AudioMixer.h"

#include "AudioSource.h"
#include "Core/Math/Scalar.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"

namespace Lumina
{
	namespace
	{
		constexpr uint32 kStartQueueCapacity   = 256;
		constexpr uint32 kCommandQueueCapacity = 1024;

		// One block at MaxRatio, plus the two frames the interpolator holds across a block boundary.
		constexpr uint32 kSourceScratchFrames = (uint32)(FAudioMixer::BlockFrames * FAudioMixer::MaxRatio) + 4;

		constexpr float kSpeedOfSound = 343.3f;
		constexpr float kMinDopplerPitch = 0.25f;
		constexpr float kMaxDopplerPitch = 4.0f;

		float AttenuationGain(const SAudioAttenuation& Attenuation, float Distance)
		{
			const float MinDistance = Math::Max(Attenuation.MinDistance, 0.0001f);
			const float MaxDistance = Math::Max(Attenuation.MaxDistance, MinDistance);
			const float Clamped     = Math::Clamp(Distance, MinDistance, MaxDistance);

			float Gain = 1.0f;
			switch (Attenuation.Model)
			{
			case EAudioAttenuationModel::None:
				Gain = 1.0f;
				break;

			case EAudioAttenuationModel::Inverse:
				Gain = MinDistance / (MinDistance + Attenuation.Rolloff * (Clamped - MinDistance));
				break;

			case EAudioAttenuationModel::Linear:
				Gain = 1.0f - Attenuation.Rolloff * (Clamped - MinDistance) / Math::Max(MaxDistance - MinDistance, 0.0001f);
				break;

			case EAudioAttenuationModel::Exponential:
				Gain = Math::Pow(Clamped / MinDistance, -Attenuation.Rolloff);
				break;
			}

			return Math::Clamp(Gain, Attenuation.MinGain, Attenuation.MaxGain);
		}

		float ConeGain(const SAudioAttenuation& Attenuation, const FVector3& SourceForward, const FVector3& SourceToListener)
		{
			if (Attenuation.ConeInnerAngle >= 360.0f)
			{
				return 1.0f;
			}

			const float ForwardLength = Math::Length(SourceForward);
			const float ToListenerLength = Math::Length(SourceToListener);
			if (ForwardLength < 0.0001f || ToListenerLength < 0.0001f)
			{
				return 1.0f;
			}

			const float CosAngle = Math::Dot(SourceForward / ForwardLength, SourceToListener / ToListenerLength);
			const float HalfInner = Math::Cos(Math::Radians(Attenuation.ConeInnerAngle) * 0.5f);
			const float HalfOuter = Math::Cos(Math::Radians(Attenuation.ConeOuterAngle) * 0.5f);

			if (CosAngle >= HalfInner)
			{
				return 1.0f;
			}
			if (CosAngle <= HalfOuter)
			{
				return Attenuation.ConeOuterGain;
			}

			const float Alpha = (CosAngle - HalfOuter) / Math::Max(HalfInner - HalfOuter, 0.0001f);
			return Math::Lerp(Attenuation.ConeOuterGain, 1.0f, Alpha);
		}

		void ConvertChannels(const float* In, uint32 InChannels, float* Out, uint32 OutChannels)
		{
			if (InChannels == OutChannels)
			{
				for (uint32 c = 0; c < OutChannels; ++c)
				{
					Out[c] = In[c];
				}
				return;
			}

			if (InChannels == 1)
			{
				for (uint32 c = 0; c < OutChannels; ++c)
				{
					Out[c] = In[0];
				}
				return;
			}

			if (OutChannels == 1)
			{
				float Sum = 0.0f;
				for (uint32 c = 0; c < InChannels; ++c)
				{
					Sum += In[c];
				}
				Out[0] = Sum / (float)InChannels;
				return;
			}

			const uint32 Shared = Math::Min(InChannels, OutChannels);
			for (uint32 c = 0; c < Shared; ++c)
			{
				Out[c] = In[c];
			}
			for (uint32 c = Shared; c < OutChannels; ++c)
			{
				Out[c] = 0.0f;
			}
		}
	}

	FAudioMixer::~FAudioMixer()
	{
		Shutdown();
	}

	bool FAudioMixer::Initialize(uint32 InSampleRate, uint32 InChannels)
	{
		LUMINA_MEMORY_SCOPE("Audio");

		if (InSampleRate == 0 || InChannels == 0)
		{
			return false;
		}

		SampleRate  = InSampleRate;
		NumChannels = Math::Min(InChannels, MaxChannels);

		const size_t BlockSamples = (size_t)BlockFrames * NumChannels;

		BusAccum.assign(BlockSamples * NumAudioBuses, 0.0f);
		MasterAccum.assign(BlockSamples, 0.0f);
		ReverbInput.assign(BlockSamples, 0.0f);
		ReverbOutput.assign(BlockSamples, 0.0f);
		VoiceScratch.assign(BlockSamples, 0.0f);
		SourceScratch.assign((size_t)kSourceScratchFrames * MaxChannels, 0.0f);

		StartQueue.Initialize(kStartQueueCapacity);
		CommandQueue.Initialize(kCommandQueueCapacity);

		for (uint32 i = 0; i < MaxVoices; ++i)
		{
			Voices[i] = FVoice();
			SlotState[i].store((uint8)EAudioVoiceState::Free, Atomic::MemoryOrderRelaxed);
			SlotFrame[i].store(0, Atomic::MemoryOrderRelaxed);
		}

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			BusVolumes[i].store(1.0f, Atomic::MemoryOrderRelaxed);
			BusMuted[i].store(false, Atomic::MemoryOrderRelaxed);
			BusPitches[i].store(1.0f, Atomic::MemoryOrderRelaxed);
			BusReverbSends[i].store(0.0f, Atomic::MemoryOrderRelaxed);
		}

		for (uint32 i = 0; i < MaxListeners; ++i)
		{
			Listeners[i] = FMixerListener();
			ListenerVersion[i].store(0, Atomic::MemoryOrderRelaxed);
		}
		Listeners[0].bEnabled = true;

		Reverb.Initialize(NumChannels, SampleRate);
		Reverb.SetParams(PendingReverbParams);

		ActiveVoices.store(0, Atomic::MemoryOrderRelaxed);
		bInitialized.store(true, Atomic::MemoryOrderRelease);
		return true;
	}

	void FAudioMixer::Shutdown()
	{
		if (!bInitialized.exchange(false, Atomic::MemoryOrderAcqRel))
		{
			return;
		}

		for (uint32 i = 0; i < MaxVoices; ++i)
		{
			Voices[i] = FVoice();
			SlotState[i].store((uint8)EAudioVoiceState::Free, Atomic::MemoryOrderRelease);
		}

		StartQueue.Shutdown();
		CommandQueue.Shutdown();

		Reverb.Shutdown();

		BusAccum.clear();
		MasterAccum.clear();
		ReverbInput.clear();
		ReverbOutput.clear();
		VoiceScratch.clear();
		SourceScratch.clear();

		ActiveVoices.store(0, Atomic::MemoryOrderRelease);
	}

	bool FAudioMixer::StartVoice(const FMixerVoiceDesc& Desc)
	{
		if (!IsInitialized() || Desc.Source == nullptr || Desc.Slot >= MaxVoices)
		{
			return false;
		}

		return StartQueue.TryEnqueue(Desc);
	}

	bool FAudioMixer::PostCommand(const FAudioCommand& Cmd)
	{
		if (!IsInitialized())
		{
			return false;
		}

		return CommandQueue.TryEnqueue(Cmd);
	}

	EAudioVoiceState FAudioMixer::GetSlotState(uint32 Slot) const
	{
		if (Slot >= MaxVoices)
		{
			return EAudioVoiceState::Free;
		}
		return (EAudioVoiceState)SlotState[Slot].load(Atomic::MemoryOrderAcquire);
	}

	uint64 FAudioMixer::GetSlotFrame(uint32 Slot) const
	{
		return Slot < MaxVoices ? SlotFrame[Slot].load(Atomic::MemoryOrderRelaxed) : 0;
	}

	void FAudioMixer::PublishSlot(uint32 Slot, EAudioVoiceState State, uint64 Frame)
	{
		SlotFrame[Slot].store(Frame, Atomic::MemoryOrderRelaxed);
		SlotState[Slot].store((uint8)State, Atomic::MemoryOrderRelease);
	}

	void FAudioMixer::SetBusVolume(EAudioBus Bus, float Volume)
	{
		BusVolumes[(uint32)Bus].store(Math::Max(Volume, 0.0f), Atomic::MemoryOrderRelaxed);
	}

	float FAudioMixer::GetBusVolume(EAudioBus Bus) const
	{
		return BusVolumes[(uint32)Bus].load(Atomic::MemoryOrderRelaxed);
	}

	void FAudioMixer::SetBusMuted(EAudioBus Bus, bool bMuted)
	{
		BusMuted[(uint32)Bus].store(bMuted, Atomic::MemoryOrderRelaxed);
	}

	bool FAudioMixer::IsBusMuted(EAudioBus Bus) const
	{
		return BusMuted[(uint32)Bus].load(Atomic::MemoryOrderRelaxed);
	}

	void FAudioMixer::SetBusPitch(EAudioBus Bus, float Pitch)
	{
		BusPitches[(uint32)Bus].store(Math::Clamp(Pitch, MinRatio, MaxRatio), Atomic::MemoryOrderRelaxed);
	}

	void FAudioMixer::SetBusReverbSend(EAudioBus Bus, float Send)
	{
		BusReverbSends[(uint32)Bus].store(Math::Clamp(Send, 0.0f, 1.0f), Atomic::MemoryOrderRelaxed);
	}

	float FAudioMixer::GetBusReverbSend(EAudioBus Bus) const
	{
		return BusReverbSends[(uint32)Bus].load(Atomic::MemoryOrderRelaxed);
	}

	void FAudioMixer::SetReverbParams(const FAudioReverbParams& Params)
	{
		PendingReverbParams = Params;
		Reverb.SetParams(Params);
	}

	FAudioReverbParams FAudioMixer::GetReverbParams() const
	{
		return Reverb.IsInitialized() ? Reverb.GetParams() : PendingReverbParams;
	}

	void FAudioMixer::SetDopplerScale(float Scale)
	{
		DopplerScale.store(Math::Max(Scale, 0.0f), Atomic::MemoryOrderRelaxed);
	}

	void FAudioMixer::SetVolumeSmoothing(float Milliseconds)
	{
		const float Frames = Math::Max(Milliseconds, 0.0f) * 0.001f * (float)SampleRate;
		GainRampFrames.store((uint32)Frames, Atomic::MemoryOrderRelaxed);
	}

	void FAudioMixer::SetMaxVoiceCount(uint32 MaxCount)
	{
		MaxActiveVoices.store(Math::Clamp(MaxCount, 1u, MaxVoices), Atomic::MemoryOrderRelaxed);
	}

	void FAudioMixer::SetListener(uint32 Index, FVector3 Position, FQuat Rotation, FVector3 Velocity)
	{
		if (Index >= MaxListeners)
		{
			return;
		}

		const FVector3 Forward = Math::Normalize(Math::Rotate(Rotation, FVector3(0.0f, 0.0f, 1.0f)));
		const FVector3 Up      = Math::Normalize(Math::Rotate(Rotation, FVector3(0.0f, 1.0f, 0.0f)));

		const uint32 Version = ListenerVersion[Index].load(Atomic::MemoryOrderRelaxed);
		ListenerVersion[Index].store(Version + 1, Atomic::MemoryOrderRelease);

		Listeners[Index].Position = Position;
		Listeners[Index].Forward  = Forward;
		Listeners[Index].Up       = Up;
		Listeners[Index].Right    = Math::Cross(Up, Forward);
		Listeners[Index].Velocity = Velocity;

		ListenerVersion[Index].store(Version + 2, Atomic::MemoryOrderRelease);
	}

	void FAudioMixer::SetListenerEnabled(uint32 Index, bool bEnabled)
	{
		if (Index >= MaxListeners)
		{
			return;
		}

		const uint32 Version = ListenerVersion[Index].load(Atomic::MemoryOrderRelaxed);
		ListenerVersion[Index].store(Version + 1, Atomic::MemoryOrderRelease);
		Listeners[Index].bEnabled = bEnabled;
		ListenerVersion[Index].store(Version + 2, Atomic::MemoryOrderRelease);
	}

	void FAudioMixer::ReadListener(uint32 Index, FMixerListener& Out) const
	{
		// Retries through a torn read rather than blocking the writer, which is the game thread.
		for (uint32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			const uint32 Before = ListenerVersion[Index].load(Atomic::MemoryOrderAcquire);
			if ((Before & 1u) != 0)
			{
				continue;
			}

			Out = Listeners[Index];

			if (ListenerVersion[Index].load(Atomic::MemoryOrderAcquire) == Before)
			{
				return;
			}
		}

		Out = FMixerListener();
	}

	void FAudioMixer::SelectListener(const FVector3& Position, FMixerListener& Out) const
	{
		FMixerListener Best;
		float BestDistance = 0.0f;
		bool bFound = false;

		for (uint32 i = 0; i < MaxListeners; ++i)
		{
			FMixerListener Candidate;
			ReadListener(i, Candidate);
			if (!Candidate.bEnabled)
			{
				continue;
			}

			const float Distance = Math::LengthSquared(Position - Candidate.Position);
			if (!bFound || Distance < BestDistance)
			{
				Best = Candidate;
				BestDistance = Distance;
				bFound = true;
			}
		}

		Out = Best;
	}

	void FAudioMixer::DrainStarts()
	{
		FMixerVoiceDesc Desc;
		while (StartQueue.TryDequeue(Desc))
		{
			if (Desc.Slot >= MaxVoices || Desc.Source == nullptr)
			{
				continue;
			}

			FVoice& Voice = Voices[Desc.Slot];

			Voice = FVoice();
			Voice.Source       = Desc.Source;
			Voice.Generation   = Desc.Generation;
			Voice.bActive      = true;
			Voice.bGenerated   = Desc.bGenerated;
			Voice.bPaused      = Desc.Params.bStartPaused;
			Voice.bSpatialized = Desc.Params.bSpatialized;
			Voice.Bus          = Desc.Params.Bus;
			Voice.BaseVolume   = Desc.Params.Volume;
			Voice.Pitch        = Desc.Params.Pitch;
			Voice.Position     = Desc.Params.Position;
			Voice.Velocity     = Desc.Params.Velocity;
			Voice.Direction    = Desc.Params.Direction;
			Voice.Attenuation  = Desc.Params.Attenuation;

			Voice.StartDelayFrames = (uint64)(Math::Max(Desc.Params.StartDelaySeconds, 0.0f) * (float)SampleRate);

			if (Desc.Params.FadeInSeconds > 0.0f)
			{
				Voice.FadeGain   = 0.0f;
				Voice.FadeTarget = 1.0f;
				Voice.FadeStep   = 1.0f / Math::Max(Desc.Params.FadeInSeconds * (float)SampleRate, 1.0f);
			}

			if (Desc.Params.bUseOcclusion)
			{
				Voice.LowPassCutoff = (float)SampleRate * 0.49f;
			}

			Voice.Source->SetLooping(Desc.Params.bLooping && !Desc.bGenerated);
			if (Desc.Params.StartFrame != 0)
			{
				Voice.Source->Seek(Desc.Params.StartFrame);
			}

			PublishSlot(Desc.Slot, Voice.bPaused ? EAudioVoiceState::Paused : EAudioVoiceState::Playing, 0);
		}
	}

	void FAudioMixer::DrainCommands()
	{
		FAudioCommand Cmd;
		while (CommandQueue.TryDequeue(Cmd))
		{
			ApplyCommand(Cmd);
		}
	}

	void FAudioMixer::DeactivateVoice(FVoice& Voice, uint32 Slot)
	{
		Voice.bActive = false;
		Voice.Source  = nullptr;
		PublishSlot(Slot, EAudioVoiceState::Free, 0);
	}

	void FAudioMixer::ApplyCommand(const FAudioCommand& Cmd)
	{
		if (Cmd.Type == EAudioCommandType::UpdateListener)
		{
			return;
		}

		if (Cmd.Type == EAudioCommandType::StopAll)
		{
			for (uint32 Slot = 0; Slot < MaxVoices; ++Slot)
			{
				FVoice& Voice = Voices[Slot];
				if (!Voice.bActive)
				{
					continue;
				}

				if (Cmd.StopMode == EAudioStopMode::AllowFadeOut && Cmd.ValueA > 0.0f)
				{
					Voice.FadeTarget     = 0.0f;
					Voice.FadeStep       = 1.0f / Math::Max(Cmd.ValueA * (float)SampleRate, 1.0f);
					Voice.bStopAfterFade = true;
				}
				else
				{
					DeactivateVoice(Voice, Slot);
				}
			}
			return;
		}

		const uint32 Slot = Cmd.Handle.Index;
		if (Slot >= MaxVoices)
		{
			return;
		}

		FVoice& Voice = Voices[Slot];
		if (!Voice.bActive || Voice.Generation != Cmd.Handle.Generation)
		{
			return;
		}

		switch (Cmd.Type)
		{
		case EAudioCommandType::StopSound:
			if (Cmd.StopMode == EAudioStopMode::AllowFadeOut && Cmd.ValueA > 0.0f)
			{
				Voice.FadeTarget     = 0.0f;
				Voice.FadeStep       = 1.0f / Math::Max(Cmd.ValueA * (float)SampleRate, 1.0f);
				Voice.bStopAfterFade = true;
			}
			else
			{
				DeactivateVoice(Voice, Slot);
			}
			break;

		case EAudioCommandType::SetVolume:     Voice.BaseVolume = Math::Max(Cmd.ValueA, 0.0f); break;
		case EAudioCommandType::SetPitch:      Voice.Pitch = Math::Clamp(Cmd.ValueA, MinRatio, MaxRatio); break;
		case EAudioCommandType::SetPosition:   Voice.Position = Cmd.Vector; break;
		case EAudioCommandType::SetVelocity:   Voice.Velocity = Cmd.Vector; break;
		case EAudioCommandType::SetDirection:  Voice.Direction = Cmd.Vector; break;
		case EAudioCommandType::SetAttenuation: Voice.Attenuation = Cmd.Attenuation; break;
		case EAudioCommandType::SetBus:        Voice.Bus = Cmd.Bus; break;
		case EAudioCommandType::SetPan:        Voice.Attenuation.Pan = Math::Clamp(Cmd.ValueA, -1.0f, 1.0f); break;

		case EAudioCommandType::SetLooping:
			if (Voice.Source != nullptr && !Voice.bGenerated)
			{
				Voice.Source->SetLooping(Cmd.bValue);
			}
			break;

		case EAudioCommandType::SetMinMaxDistance:
			Voice.Attenuation.MinDistance = Cmd.ValueA;
			Voice.Attenuation.MaxDistance = Cmd.ValueB;
			break;

		case EAudioCommandType::SetPaused:
			Voice.bPaused = Cmd.bValue;
			PublishSlot(Slot, Voice.bPaused ? EAudioVoiceState::Paused : EAudioVoiceState::Playing, Voice.PlayedFrames);
			break;

		case EAudioCommandType::SetOcclusion:
		{
			const float Amount = Math::Clamp(Cmd.ValueA, 0.0f, 1.0f);
			const float Nyquist = (float)SampleRate * 0.49f;
			Voice.OcclusionGain = Math::Lerp(1.0f, Math::Clamp(Cmd.ValueC, 0.0f, 1.0f), Amount);
			Voice.LowPassCutoff = Math::Lerp(Nyquist, Math::Clamp(Cmd.ValueB, 20.0f, Nyquist), Amount);
			break;
		}

		case EAudioCommandType::SetLowPassCutoff:
			Voice.LowPassCutoff = Cmd.ValueA <= 0.0f ? 0.0f : Math::Clamp(Cmd.ValueA, 20.0f, (float)SampleRate * 0.49f);
			break;

		case EAudioCommandType::FadeTo:
			Voice.FadeTarget     = Math::Max(Cmd.ValueA, 0.0f);
			Voice.FadeStep       = 1.0f / Math::Max(Cmd.ValueB * (float)SampleRate, 1.0f);
			Voice.bStopAfterFade = false;
			break;

		case EAudioCommandType::SeekToFrame:
			if (Voice.Source != nullptr && Voice.Source->IsSeekable())
			{
				Voice.Source->Seek(Cmd.FrameValue);
				Voice.bPrimed = false;
				Voice.bSourceDrained = false;
				Voice.bHeldLast = false;
				Voice.ResamplePos = 0.0;
			}
			break;

		default:
			break;
		}
	}

	float FAudioMixer::ComputeVoicePitch(const FVoice& Voice) const
	{
		float Pitch = Voice.Pitch * BusPitches[(uint32)Voice.Bus].load(Atomic::MemoryOrderRelaxed);

		const float Doppler = Voice.Attenuation.DopplerFactor * DopplerScale.load(Atomic::MemoryOrderRelaxed);
		if (Voice.bSpatialized && Doppler > 0.0f)
		{
			FMixerListener Listener;
			SelectListener(Voice.Position, Listener);

			const FVector3 ToVoice = Voice.Position - Listener.Position;
			const float Distance = Math::Length(ToVoice);
			if (Distance > 0.0001f)
			{
				const FVector3 Direction = ToVoice / Distance;
				const float ListenerSpeed = Math::Dot(Listener.Velocity, Direction);
				const float VoiceSpeed    = Math::Dot(Voice.Velocity, Direction);

				const float Denominator = kSpeedOfSound + VoiceSpeed;
				if (Math::Abs(Denominator) > 0.0001f)
				{
					const float Shift = (kSpeedOfSound + ListenerSpeed) / Denominator;
					Pitch *= Math::Clamp(1.0f + Doppler * (Shift - 1.0f), kMinDopplerPitch, kMaxDopplerPitch);
				}
			}
		}

		return Pitch;
	}

	void FAudioMixer::ComputeVoiceGains(const FVoice& Voice, float* OutGains) const
	{
		float Gain = Voice.BaseVolume * Voice.OcclusionGain * Voice.FadeGain;
		float Pan  = Math::Clamp(Voice.Attenuation.Pan, -1.0f, 1.0f);

		if (Voice.bSpatialized)
		{
			FMixerListener Listener;
			SelectListener(Voice.Position, Listener);

			const FVector3 ToVoice = Voice.Attenuation.Positioning == EAudioPositioning::Relative
				? Voice.Position
				: Voice.Position - Listener.Position;

			const float Distance = Math::Length(ToVoice);
			Gain *= AttenuationGain(Voice.Attenuation, Distance);
			Gain *= ConeGain(Voice.Attenuation, Voice.Direction, -ToVoice);

			if (Distance > 0.0001f)
			{
				const FVector3 Direction = ToVoice / Distance;

				if (Voice.Attenuation.DirectionalFactor > 0.0f)
				{
					const float Facing = Math::Dot(Listener.Forward, Direction);
					Gain *= Math::Lerp(1.0f, Math::Clamp(Facing * 0.5f + 0.5f, 0.0f, 1.0f), Voice.Attenuation.DirectionalFactor);
				}

				Pan = Math::Clamp(Pan + Math::Dot(Direction, Listener.Right), -1.0f, 1.0f);
			}
		}

		if (NumChannels == 2)
		{
			// Balance rather than constant power, so a centered voice is not quieter than a hard panned one.
			OutGains[0] = Gain * (Pan > 0.0f ? 1.0f - Pan : 1.0f);
			OutGains[1] = Gain * (Pan < 0.0f ? 1.0f + Pan : 1.0f);
			return;
		}

		for (uint32 c = 0; c < NumChannels; ++c)
		{
			OutGains[c] = Gain;
		}
	}

	uint32 FAudioMixer::ResampleVoice(FVoice& Voice, uint32 Frames, float Ratio)
	{
		const uint32 SourceChannels = Math::Clamp(Voice.Source->GetChannelCount(), 1u, MaxChannels);

		if (!Voice.bPrimed)
		{
			Voice.ResamplePos = 0.0;
		}

		// Exactly what the interpolator will consume, because a frame the loop never reaches is lost.
		const double Advances = Math::Max(Math::Floor(Voice.ResamplePos + (double)(Frames - 1) * (double)Ratio), 0.0);
		const uint32 PrimeFrames = Voice.bPrimed ? 0u : 2u;

		const uint32 SourceNeeded = Math::Min((uint32)Advances + PrimeFrames, kSourceScratchFrames);

		uint32 SourceAvailable = 0;
		if (SourceNeeded > 0)
		{
			SourceAvailable = Voice.Source->Pull(SourceScratch.data(), SourceNeeded);
			if (SourceAvailable < SourceNeeded)
			{
				Voice.bSourceDrained = true;
			}
		}

		uint32 SourceIndex = 0;

		if (!Voice.bPrimed)
		{
			if (SourceAvailable == 0)
			{
				Voice.bSourceDrained = true;
				return 0;
			}

			// Alpha zero emits PrevFrame, so the pair has to straddle the first source frame from the start.
			const float* First  = SourceScratch.data();
			const float* Second = SourceAvailable > 1 ? First + SourceChannels : First;

			for (uint32 c = 0; c < SourceChannels; ++c)
			{
				Voice.PrevFrame[c] = First[c];
				Voice.NextFrame[c] = Second[c];
			}

			SourceIndex = Math::Min(2u, SourceAvailable);
			Voice.bPrimed = true;
			Voice.bHeldLast = false;
		}

		uint32 Produced = 0;
		float Interpolated[MaxChannels] = {};

		for (uint32 Frame = 0; Frame < Frames; ++Frame)
		{
			bool bRanDry = false;
			while (Voice.ResamplePos >= 1.0)
			{
				if (SourceIndex < SourceAvailable)
				{
					const float* At = SourceScratch.data() + (size_t)SourceIndex * SourceChannels;
					for (uint32 c = 0; c < SourceChannels; ++c)
					{
						Voice.PrevFrame[c] = Voice.NextFrame[c];
						Voice.NextFrame[c] = At[c];
					}
					++SourceIndex;
				}
				else if (!Voice.bHeldLast)
				{
					// One held step, so the final source frame still reaches the output instead of being dropped.
					for (uint32 c = 0; c < SourceChannels; ++c)
					{
						Voice.PrevFrame[c] = Voice.NextFrame[c];
					}
					Voice.bHeldLast = true;
				}
				else
				{
					bRanDry = true;
					break;
				}

				Voice.ResamplePos -= 1.0;
			}

			if (bRanDry)
			{
				Voice.bSourceDrained = true;
				break;
			}

			const float Alpha = (float)Voice.ResamplePos;
			for (uint32 c = 0; c < SourceChannels; ++c)
			{
				Interpolated[c] = Voice.PrevFrame[c] + (Voice.NextFrame[c] - Voice.PrevFrame[c]) * Alpha;
			}

			ConvertChannels(Interpolated, SourceChannels, VoiceScratch.data() + (size_t)Frame * NumChannels, NumChannels);

			Voice.ResamplePos += (double)Ratio;
			++Produced;
		}

		return Produced;
	}

	void FAudioMixer::RenderVoice(FVoice& Voice, uint32 Frames)
	{
		const uint32 SourceRate = Voice.Source->GetSampleRate();
		if (SourceRate == 0)
		{
			Voice.bSourceDrained = true;
			return;
		}

		const float Ratio = Math::Clamp(
			(float)SourceRate / (float)SampleRate * ComputeVoicePitch(Voice), MinRatio, MaxRatio);

		const uint32 Produced = ResampleVoice(Voice, Frames, Ratio);
		if (Produced == 0)
		{
			return;
		}

		float TargetGains[MaxChannels] = {};
		ComputeVoiceGains(Voice, TargetGains);

		if (!Voice.bGainPrimed)
		{
			for (uint32 c = 0; c < NumChannels; ++c)
			{
				Voice.CurrentGain[c] = TargetGains[c];
			}
			Voice.bGainPrimed = true;
		}

		// A gain edit ramps across the block instead of stepping, which is what removes the click.
		const uint32 RampFrames = Math::Max(GainRampFrames.load(Atomic::MemoryOrderRelaxed), 1u);
		const float BlockAlpha = Math::Min((float)Produced / (float)RampFrames, 1.0f);

		float StartGain[MaxChannels] = {};
		float EndGain[MaxChannels] = {};
		for (uint32 c = 0; c < NumChannels; ++c)
		{
			StartGain[c] = Voice.CurrentGain[c];
			EndGain[c]   = Voice.CurrentGain[c] + (TargetGains[c] - Voice.CurrentGain[c]) * BlockAlpha;
			Voice.CurrentGain[c] = EndGain[c];
		}

		const bool bFiltered = Voice.LowPassCutoff > 0.0f && Voice.LowPassCutoff < (float)SampleRate * 0.49f;
		const float FilterAlpha = bFiltered
			? Math::Clamp(1.0f - Math::Exp(-2.0f * Math::Pi<float>() * Voice.LowPassCutoff / (float)SampleRate), 0.0f, 1.0f)
			: 1.0f;

		float* Bus = BusAccum.data() + (size_t)(uint32)Voice.Bus * BlockFrames * NumChannels;
		const float InvProduced = 1.0f / (float)Produced;

		for (uint32 Frame = 0; Frame < Produced; ++Frame)
		{
			const float Alpha = (float)Frame * InvProduced;

			if (Voice.FadeStep > 0.0f && Voice.FadeGain != Voice.FadeTarget)
			{
				const float Delta = Voice.FadeTarget - Voice.FadeGain;
				const float Step  = Voice.FadeStep;
				Voice.FadeGain += Delta > 0.0f ? Math::Min(Step, Delta) : Math::Max(-Step, Delta);
			}

			for (uint32 c = 0; c < NumChannels; ++c)
			{
				const size_t Index = (size_t)Frame * NumChannels + c;
				float Sample = VoiceScratch[Index];

				if (bFiltered)
				{
					Voice.LowPassState[c] += FilterAlpha * (Sample - Voice.LowPassState[c]);
					Sample = Voice.LowPassState[c];
				}

				Bus[Index] += Sample * (StartGain[c] + (EndGain[c] - StartGain[c]) * Alpha);
			}
		}

		Voice.PlayedFrames += Produced;
	}

	void FAudioMixer::RenderBlock(float* Out, uint32 Frames)
	{
		const size_t BlockSamples = (size_t)Frames * NumChannels;
		const size_t StrideSamples = (size_t)BlockFrames * NumChannels;

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			Memory::Memzero(BusAccum.data() + (size_t)i * StrideSamples, BlockSamples * sizeof(float));
		}

		uint32 Active = 0;
		const uint32 VoiceBudget = MaxActiveVoices.load(Atomic::MemoryOrderRelaxed);

		for (uint32 Slot = 0; Slot < MaxVoices; ++Slot)
		{
			FVoice& Voice = Voices[Slot];
			if (!Voice.bActive)
			{
				continue;
			}

			if (Voice.bPaused)
			{
				++Active;
				continue;
			}

			if (Voice.StartDelayFrames > 0)
			{
				Voice.StartDelayFrames -= Math::Min(Voice.StartDelayFrames, (uint64)Frames);
				++Active;
				continue;
			}

			if (Active >= VoiceBudget)
			{
				continue;
			}

			RenderVoice(Voice, Frames);
			++Active;

			const bool bFadedOut = Voice.bStopAfterFade && Voice.FadeGain <= 0.0f;
			const bool bEnded    = !Voice.bGenerated && Voice.bSourceDrained && Voice.Source->IsAtEnd();
			const bool bGraphEnded = Voice.bGenerated && Voice.Source->IsAtEnd();

			if (bFadedOut || bEnded || bGraphEnded)
			{
				DeactivateVoice(Voice, Slot);
				--Active;
			}
			else
			{
				PublishSlot(Slot, EAudioVoiceState::Playing, Voice.Source->GetCursor());
			}
		}

		ActiveVoices.store(Active, Atomic::MemoryOrderRelaxed);

		Memory::Memzero(MasterAccum.data(), BlockSamples * sizeof(float));
		Memory::Memzero(ReverbInput.data(), BlockSamples * sizeof(float));

		bool bAnyReverb = false;

		for (uint32 i = 0; i < NumAudioBuses; ++i)
		{
			const float* Source = BusAccum.data() + (size_t)i * StrideSamples;

			// Master is summed raw here; its own volume lands on the final mix once, at the end.
			const float Gain = (i == (uint32)EAudioBus::Master)
				? 1.0f
				: (BusMuted[i].load(Atomic::MemoryOrderRelaxed) ? 0.0f : BusVolumes[i].load(Atomic::MemoryOrderRelaxed));

			if (Gain <= 0.0f)
			{
				continue;
			}

			const float Send = BusReverbSends[i].load(Atomic::MemoryOrderRelaxed);
			for (size_t s = 0; s < BlockSamples; ++s)
			{
				MasterAccum[s] += Source[s] * Gain;
			}

			if (Send > 0.0f)
			{
				bAnyReverb = true;
				const float WetGain = Gain * Send;
				for (size_t s = 0; s < BlockSamples; ++s)
				{
					ReverbInput[s] += Source[s] * WetGain;
				}
			}
		}

		if (bAnyReverb && Reverb.IsInitialized())
		{
			Reverb.Process(ReverbInput.data(), ReverbOutput.data(), Frames);
			for (size_t s = 0; s < BlockSamples; ++s)
			{
				MasterAccum[s] += ReverbOutput[s];
			}
		}

		const float MasterGain = BusMuted[(uint32)EAudioBus::Master].load(Atomic::MemoryOrderRelaxed)
			? 0.0f
			: BusVolumes[(uint32)EAudioBus::Master].load(Atomic::MemoryOrderRelaxed);

		for (size_t s = 0; s < BlockSamples; ++s)
		{
			Out[s] = Math::Clamp(MasterAccum[s] * MasterGain, -1.0f, 1.0f);
		}
	}

	void FAudioMixer::RenderAudio(float* Out, uint32 FrameCount)
	{
		if (Out == nullptr || FrameCount == 0)
		{
			return;
		}

		if (!IsInitialized())
		{
			Memory::Memzero(Out, (size_t)FrameCount * NumChannels * sizeof(float));
			return;
		}

		DrainStarts();
		DrainCommands();

		uint32 Rendered = 0;
		while (Rendered < FrameCount)
		{
			const uint32 Frames = Math::Min(BlockFrames, FrameCount - Rendered);
			RenderBlock(Out + (size_t)Rendered * NumChannels, Frames);
			Rendered += Frames;
		}

		RenderCount.fetch_add(1, Atomic::MemoryOrderRelease);
	}
}
