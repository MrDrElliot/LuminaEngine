#pragma once

#include "Containers/Array.h"
#include "Core/Threading/Atomic.h"
#include "Platform/GenericPlatform.h"
#include "MiniAudio/miniaudio.h"

namespace Lumina
{
	/** Runtime reverb parameters. Every field is atomically published to the mixer. */
	struct FAudioReverbParams
	{
		/** Decay length. 0 = tiny room, 1 = cathedral. */
		float RoomSize = 0.5f;

		/** High-frequency absorption in the tail. */
		float Damping = 0.5f;

		/** Stereo spread of the tail. Ignored for non-stereo output. */
		float Width = 1.0f;

		/** Gain of the reverb return. 0 = silent. */
		float WetLevel = 0.35f;
	};

	// Freeverb-style comb/allpass network exposed as a miniaudio node. Emits the wet signal only; the
	// dry path is a separate branch of the splitter that feeds it, so wet/dry is a graph-level mix.
	class FAudioReverbNode
	{
	public:

		ma_result Init(ma_node_graph* NodeGraph, uint32 Channels, uint32 SampleRate, const ma_allocation_callbacks* Callbacks);
		void Uninit(const ma_allocation_callbacks* Callbacks);

		void SetParams(const FAudioReverbParams& InParams);
		FAudioReverbParams GetParams() const;

		ma_node* GetNode() { return &Base; }
		bool IsInitialized() const { return bInitialized; }

	private:

		static void OnProcess(ma_node* Node, const float** FramesIn, uint32* FrameCountIn, float** FramesOut, uint32* FrameCountOut);
		void Process(const float* In, float* Out, uint32 FrameCount);

		struct FCombFilter
		{
			TVector<float> Buffer;
			uint32 Index = 0;
			float FilterStore = 0.0f;

			float Step(float Input, float Feedback, float Damp1, float Damp2)
			{
				const float Output = Buffer[Index];
				FilterStore = (Output * Damp2) + (FilterStore * Damp1);
				Buffer[Index] = Input + (FilterStore * Feedback);
				if (++Index >= Buffer.size())
				{
					Index = 0;
				}
				return Output;
			}
		};

		struct FAllpassFilter
		{
			TVector<float> Buffer;
			uint32 Index = 0;

			float Step(float Input)
			{
				const float Buffered = Buffer[Index];
				const float Output = Buffered - Input;
				Buffer[Index] = Input + (Buffered * 0.5f);
				if (++Index >= Buffer.size())
				{
					Index = 0;
				}
				return Output;
			}
		};

		static constexpr uint32 NumCombs   = 8;
		static constexpr uint32 NumAllpass = 4;
		static constexpr uint32 MaxChannels = 8;

		struct FChannelNetwork
		{
			FCombFilter Combs[NumCombs];
			FAllpassFilter Allpass[NumAllpass];
		};

		ma_node_base Base{};
		FChannelNetwork Networks[MaxChannels];

		uint32 ChannelCount = 0;
		bool bInitialized = false;

		TAtomic<float> RoomSize{0.5f};
		TAtomic<float> Damping{0.5f};
		TAtomic<float> Width{1.0f};
		TAtomic<float> WetLevel{0.35f};
	};
}
