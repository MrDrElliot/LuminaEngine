#pragma once

#include "AudioTypes.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"

namespace Lumina
{
	// Game thread to audio pump control message. Flat rather than a union so payloads with default
	// member initializers (SAudioAttenuation) can live here.
	struct FAudioCommand
	{
		EAudioCommandType Type = EAudioCommandType::StopSound;
		FAudioHandle Handle;

		float ValueA = 0.0f;
		float ValueB = 0.0f;
		float ValueC = 0.0f;
		bool bValue = false;
		uint8 ByteValue = 0;
		uint64 FrameValue = 0;

		FVector3 Vector = FVector3(0.0f);
		FVector3 VectorB = FVector3(0.0f);
		FQuat Rotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
		EAudioBus Bus = EAudioBus::SFX;
		EAudioStopMode StopMode = EAudioStopMode::Immediate;
		SAudioAttenuation Attenuation;

		static FAudioCommand Make(EAudioCommandType Type, FAudioHandle Handle)
		{
			FAudioCommand Cmd;
			Cmd.Type   = Type;
			Cmd.Handle = Handle;
			return Cmd;
		}

		static FAudioCommand MakeStop(FAudioHandle Handle, EAudioStopMode Mode, float FadeSeconds)
		{
			FAudioCommand Cmd = Make(EAudioCommandType::StopSound, Handle);
			Cmd.StopMode = Mode;
			Cmd.ValueA   = FadeSeconds;
			return Cmd;
		}

		static FAudioCommand MakeStopAll(EAudioStopMode Mode, float FadeSeconds)
		{
			FAudioCommand Cmd = Make(EAudioCommandType::StopAll, FAudioHandle::Invalid());
			Cmd.StopMode = Mode;
			Cmd.ValueA   = FadeSeconds;
			return Cmd;
		}

		static FAudioCommand MakeFloat(EAudioCommandType Type, FAudioHandle Handle, float Value)
		{
			FAudioCommand Cmd = Make(Type, Handle);
			Cmd.ValueA = Value;
			return Cmd;
		}

		static FAudioCommand MakeFloat2(EAudioCommandType Type, FAudioHandle Handle, float A, float B)
		{
			FAudioCommand Cmd = Make(Type, Handle);
			Cmd.ValueA = A;
			Cmd.ValueB = B;
			return Cmd;
		}

		static FAudioCommand MakeBool(EAudioCommandType Type, FAudioHandle Handle, bool Value)
		{
			FAudioCommand Cmd = Make(Type, Handle);
			Cmd.bValue = Value;
			return Cmd;
		}

		static FAudioCommand MakeVector(EAudioCommandType Type, FAudioHandle Handle, const FVector3& Value)
		{
			FAudioCommand Cmd = Make(Type, Handle);
			Cmd.Vector = Value;
			return Cmd;
		}

		static FAudioCommand MakeAttenuation(FAudioHandle Handle, const SAudioAttenuation& Value)
		{
			FAudioCommand Cmd = Make(EAudioCommandType::SetAttenuation, Handle);
			Cmd.Attenuation = Value;
			return Cmd;
		}

		static FAudioCommand MakeSeekToFrame(FAudioHandle Handle, uint64 Frame)
		{
			FAudioCommand Cmd = Make(EAudioCommandType::SeekToFrame, Handle);
			Cmd.FrameValue = Frame;
			return Cmd;
		}

		static FAudioCommand MakeSetBus(FAudioHandle Handle, EAudioBus Bus)
		{
			FAudioCommand Cmd = Make(EAudioCommandType::SetBus, Handle);
			Cmd.Bus = Bus;
			return Cmd;
		}

		static FAudioCommand MakeSetPriority(FAudioHandle Handle, uint8 Priority)
		{
			FAudioCommand Cmd = Make(EAudioCommandType::SetPriority, Handle);
			Cmd.ByteValue = Priority;
			return Cmd;
		}

		static FAudioCommand MakeUpdateListener(uint32 ListenerIndex, const FVector3& Position, const FQuat& Rotation, const FVector3& Velocity)
		{
			FAudioCommand Cmd = Make(EAudioCommandType::UpdateListener, FAudioHandle::Invalid());
			Cmd.ByteValue = (uint8)ListenerIndex;
			Cmd.Vector    = Position;
			Cmd.VectorB   = Velocity;
			Cmd.Rotation  = Rotation;
			return Cmd;
		}
	};
}
