#include "RuntimePCH.h"
#include "AudioReverb.h"

#include "Core/Math/Math.h"

namespace Lumina
{
	namespace
	{
		// Freeverb tunings, authored against 44.1 kHz and scaled to the device rate on init.
		constexpr uint32 CombTuning[8]    = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
		constexpr uint32 AllpassTuning[4] = { 556, 441, 341, 225 };
		constexpr uint32 StereoSpread     = 23;
		constexpr uint32 TuningSampleRate = 44100;

		constexpr float FixedGain  = 0.015f;
		constexpr float ScaleDamp  = 0.4f;
		constexpr float ScaleRoom  = 0.28f;
		constexpr float OffsetRoom = 0.7f;
	}

	ma_result FAudioReverbNode::Init(ma_node_graph* NodeGraph, uint32 Channels, uint32 SampleRate, const ma_allocation_callbacks* Callbacks)
	{
		ChannelCount = Math::Min(Channels, MaxChannels);
		if (ChannelCount == 0)
		{
			return MA_INVALID_ARGS;
		}

		const double RateScale = (double)SampleRate / (double)TuningSampleRate;

		for (uint32 Channel = 0; Channel < ChannelCount; ++Channel)
		{
			// Odd channels get the freeverb stereo offset so the tails decorrelate.
			const uint32 Spread = (Channel & 1u) ? StereoSpread : 0u;

			for (uint32 i = 0; i < NumCombs; ++i)
			{
				const uint32 Size = Math::Max((uint32)((CombTuning[i] + Spread) * RateScale), 1u);
				Networks[Channel].Combs[i].Buffer.assign(Size, 0.0f);
				Networks[Channel].Combs[i].Index = 0;
				Networks[Channel].Combs[i].FilterStore = 0.0f;
			}

			for (uint32 i = 0; i < NumAllpass; ++i)
			{
				const uint32 Size = Math::Max((uint32)((AllpassTuning[i] + Spread) * RateScale), 1u);
				Networks[Channel].Allpass[i].Buffer.assign(Size, 0.0f);
				Networks[Channel].Allpass[i].Index = 0;
			}
		}

		// Function-local so the private OnProcess is reachable; the vtable is shared by every instance.
		static ma_node_vtable VTable =
		{
			&FAudioReverbNode::OnProcess,
			nullptr,
			1,
			1,
			MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT,
		};

		ma_node_config Config = ma_node_config_init();
		Config.vtable          = &VTable;
		Config.pInputChannels  = &ChannelCount;
		Config.pOutputChannels = &ChannelCount;

		const ma_result Result = ma_node_init(NodeGraph, &Config, Callbacks, &Base);
		if (Result != MA_SUCCESS)
		{
			return Result;
		}

		bInitialized = true;
		return MA_SUCCESS;
	}

	void FAudioReverbNode::Uninit(const ma_allocation_callbacks* Callbacks)
	{
		if (!bInitialized)
		{
			return;
		}

		ma_node_uninit(&Base, Callbacks);
		bInitialized = false;

		for (uint32 Channel = 0; Channel < ChannelCount; ++Channel)
		{
			for (FCombFilter& Comb : Networks[Channel].Combs)
			{
				Comb.Buffer.clear();
			}
			for (FAllpassFilter& Allpass : Networks[Channel].Allpass)
			{
				Allpass.Buffer.clear();
			}
		}
	}

	void FAudioReverbNode::SetParams(const FAudioReverbParams& InParams)
	{
		RoomSize.store(Math::Clamp(InParams.RoomSize, 0.0f, 1.0f), Atomic::MemoryOrderRelaxed);
		Damping.store(Math::Clamp(InParams.Damping, 0.0f, 1.0f), Atomic::MemoryOrderRelaxed);
		Width.store(Math::Clamp(InParams.Width, 0.0f, 1.0f), Atomic::MemoryOrderRelaxed);
		WetLevel.store(Math::Max(InParams.WetLevel, 0.0f), Atomic::MemoryOrderRelaxed);
	}

	FAudioReverbParams FAudioReverbNode::GetParams() const
	{
		FAudioReverbParams Out;
		Out.RoomSize = RoomSize.load(Atomic::MemoryOrderRelaxed);
		Out.Damping  = Damping.load(Atomic::MemoryOrderRelaxed);
		Out.Width    = Width.load(Atomic::MemoryOrderRelaxed);
		Out.WetLevel = WetLevel.load(Atomic::MemoryOrderRelaxed);
		return Out;
	}

	void FAudioReverbNode::OnProcess(ma_node* Node, const float** FramesIn, uint32* FrameCountIn, float** FramesOut, uint32* FrameCountOut)
	{
		FAudioReverbNode* Self = reinterpret_cast<FAudioReverbNode*>(Node);
		Self->Process(FramesIn != nullptr ? FramesIn[0] : nullptr, FramesOut[0], *FrameCountOut);
		*FrameCountIn = *FrameCountOut;
	}

	void FAudioReverbNode::Process(const float* In, float* Out, uint32 FrameCount)
	{
		const uint32 Channels = ChannelCount;
		const float Feedback  = (RoomSize.load(Atomic::MemoryOrderRelaxed) * ScaleRoom) + OffsetRoom;
		const float Damp1     = Damping.load(Atomic::MemoryOrderRelaxed) * ScaleDamp;
		const float Damp2     = 1.0f - Damp1;
		const float Wet       = WetLevel.load(Atomic::MemoryOrderRelaxed);
		const float WidthMix  = Width.load(Atomic::MemoryOrderRelaxed);

		const float Wet1 = Wet * (WidthMix * 0.5f + 0.5f);
		const float Wet2 = Wet * ((1.0f - WidthMix) * 0.5f);

		for (uint32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			float Tail[MaxChannels];

			for (uint32 Channel = 0; Channel < Channels; ++Channel)
			{
				const float Input = (In != nullptr ? In[Frame * Channels + Channel] : 0.0f) * FixedGain;

				float Accum = 0.0f;
				for (FCombFilter& Comb : Networks[Channel].Combs)
				{
					Accum += Comb.Step(Input, Feedback, Damp1, Damp2);
				}

				for (FAllpassFilter& Allpass : Networks[Channel].Allpass)
				{
					Accum = Allpass.Step(Accum);
				}

				Tail[Channel] = Accum;
			}

			if (Channels == 2)
			{
				Out[Frame * 2 + 0] = Tail[0] * Wet1 + Tail[1] * Wet2;
				Out[Frame * 2 + 1] = Tail[1] * Wet1 + Tail[0] * Wet2;
			}
			else
			{
				for (uint32 Channel = 0; Channel < Channels; ++Channel)
				{
					Out[Frame * Channels + Channel] = Tail[Channel] * Wet;
				}
			}
		}
	}
}
