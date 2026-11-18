#include "RuntimePCH.h"
#include "QoaCodec.h"

#include "Core/Math/Math.h"
#include "Memory/Memcpy.h"
#include "Memory/MemoryTracking.h"

namespace Lumina::Audio
{
	namespace QoaInternal
	{
		constexpr uint32 kMagic          = 0x716f6166; // "qoaf"
		constexpr uint32 kFileHeaderSize = 8;

		constexpr int32 kScaleFactors[16] = { 1, 7, 21, 45, 84, 138, 211, 304, 421, 562, 731, 928, 1157, 1419, 1715, 2048 };

		// Index is the residual divided by the scale factor and clamped to [-8, 8], value the 3 bit code nearest to it.
		constexpr int32 kQuantize[17] = { 7, 7, 7, 5, 5, 3, 3, 1, 0, 0, 2, 2, 4, 4, 6, 6, 6 };

		constexpr double kDequantSteps[8] = { 0.75, -0.75, 2.5, -2.5, 4.5, -4.5, 7.0, -7.0 };

		struct FTables
		{
			int32 Dequant[16][8] = {};
			int32 Reciprocal[16] = {};

			constexpr FTables()
			{
				for (int32 Scale = 0; Scale < 16; ++Scale)
				{
					Reciprocal[Scale] = ((1 << 16) + kScaleFactors[Scale] - 1) / kScaleFactors[Scale];
					for (int32 Code = 0; Code < 8; ++Code)
					{
						const double Value = kScaleFactors[Scale] * kDequantSteps[Code];
						Dequant[Scale][Code] = static_cast<int32>(Value < 0.0 ? Value - 0.5 : Value + 0.5);
					}
				}
			}
		};

		constexpr FTables kTables;

		struct FLms
		{
			int32 History[4] = { 0, 0, 0, 0 };
			int32 Weights[4] = { 0, 0, -(1 << 13), 1 << 14 };

			int32 Predict() const
			{
				int64 Prediction = 0;
				for (int32 i = 0; i < 4; ++i)
				{
					Prediction += static_cast<int64>(Weights[i]) * History[i];
				}
				return static_cast<int32>(Prediction >> 13);
			}

			void Update(int32 Sample, int32 Residual)
			{
				const int32 Delta = Residual >> 4;
				for (int32 i = 0; i < 4; ++i)
				{
					Weights[i] += History[i] < 0 ? -Delta : Delta;
				}
				History[0] = History[1];
				History[1] = History[2];
				History[2] = History[3];
				History[3] = Sample;
			}
		};

		int32 ClampS16(int32 Value)
		{
			return Math::Clamp(Value, -32768, 32767);
		}

		// Rounds away from zero and never collapses a non-zero residual to zero, so small errors still steer the predictor.
		int32 Divide(int32 Value, int32 Scale)
		{
			const int64 Product = static_cast<int64>(Value) * kTables.Reciprocal[Scale];
			int32 N = static_cast<int32>((Product + (1 << 15)) >> 16);
			N = N + ((Value > 0) - (Value < 0)) - ((N > 0) - (N < 0));
			return N;
		}

		uint64 ReadU64(const uint8* P)
		{
			uint64 Value = 0;
			for (int32 i = 0; i < 8; ++i)
			{
				Value = (Value << 8) | P[i];
			}
			return Value;
		}

		void WriteU64(TVector<uint8>& Out, uint64 Value)
		{
			for (int32 Shift = 56; Shift >= 0; Shift -= 8)
			{
				Out.Add(static_cast<uint8>(Value >> Shift));
			}
		}

		uint64 FullFrameSize(uint32 NumChannels)
		{
			return 8ull + 16ull * NumChannels + 8ull * Qoa::kSlicesPerFrame * NumChannels;
		}
	}

	bool Qoa::IsQoa(const void* Data, size_t Size)
	{
		return Data != nullptr && Size >= QoaInternal::kFileHeaderSize
			&& static_cast<uint32>(QoaInternal::ReadU64(static_cast<const uint8*>(Data)) >> 32) == QoaInternal::kMagic;
	}

	bool Qoa::Encode(const float* Interleaved, uint64 NumFrames, uint32 NumChannels, uint32 SampleRate, TVector<uint8>& Out)
	{
		using namespace QoaInternal;
		LUMINA_MEMORY_SCOPE("Audio");

		if (Interleaved == nullptr || NumFrames == 0 || NumFrames > 0xFFFFFFFFull || NumChannels == 0 || NumChannels > kMaxChannels
			|| SampleRate == 0 || SampleRate > 0xFFFFFF)
		{
			return false;
		}

		const uint64 NumFramesInFile = (NumFrames + kFrameLen - 1) / kFrameLen;
		Out.Reset();
		Out.Reserve(IntCastChecked<int32>(kFileHeaderSize + NumFramesInFile * FullFrameSize(NumChannels)));
		WriteU64(Out, (static_cast<uint64>(kMagic) << 32) | NumFrames);

		FLms Lms[kMaxChannels];
		for (uint64 FrameStart = 0; FrameStart < NumFrames; FrameStart += kFrameLen)
		{
			const uint32 FrameLen  = static_cast<uint32>(Math::Min<uint64>(kFrameLen, NumFrames - FrameStart));
			const uint32 NumSlices = (FrameLen + kSliceLen - 1) / kSliceLen;
			const uint64 FrameSize = 8ull + 16ull * NumChannels + 8ull * NumSlices * NumChannels;

			WriteU64(Out, (static_cast<uint64>(NumChannels) << 56) | (static_cast<uint64>(SampleRate) << 32)
				| (static_cast<uint64>(FrameLen) << 16) | FrameSize);

			for (uint32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				uint64 History = 0;
				uint64 Weights = 0;
				for (int32 i = 0; i < 4; ++i)
				{
					History = (History << 16) | (static_cast<uint64>(Lms[Channel].History[i]) & 0xFFFF);
					Weights = (Weights << 16) | (static_cast<uint64>(Lms[Channel].Weights[i]) & 0xFFFF);
				}
				WriteU64(Out, History);
				WriteU64(Out, Weights);
			}

			for (uint32 SliceStart = 0; SliceStart < FrameLen; SliceStart += kSliceLen)
			{
				const uint32 SliceLen = Math::Min(kSliceLen, FrameLen - SliceStart);
				for (uint32 Channel = 0; Channel < NumChannels; ++Channel)
				{
					uint64 BestError = ~0ull;
					uint64 BestSlice = 0;
					FLms   BestLms;

					// Every scale factor is tried, and the one with the least error plus weight growth wins.
					for (int32 Scale = 0; Scale < 16; ++Scale)
					{
						FLms   Trial = Lms[Channel];
						uint64 Slice = static_cast<uint64>(Scale);
						uint64 Error = 0;

						for (uint32 i = 0; i < SliceLen; ++i)
						{
							const float  Source    = Interleaved[(FrameStart + SliceStart + i) * NumChannels + Channel];
							const int32  Sample    = ClampS16(static_cast<int32>(Math::Round(Math::Clamp(Source, -1.0f, 1.0f) * 32767.0f)));
							const int32  Predicted = Trial.Predict();
							const int32  Scaled    = Divide(Sample - Predicted, Scale);
							const int32  Code      = kQuantize[Math::Clamp(Scaled, -8, 8) + 8];
							const int32  Dequant   = kTables.Dequant[Scale][Code];
							const int32  Rebuilt   = ClampS16(Predicted + Dequant);

							int64 WeightsPenalty = ((static_cast<int64>(Trial.Weights[0]) * Trial.Weights[0] + static_cast<int64>(Trial.Weights[1]) * Trial.Weights[1]
								+ static_cast<int64>(Trial.Weights[2]) * Trial.Weights[2] + static_cast<int64>(Trial.Weights[3]) * Trial.Weights[3]) >> 18) - 0x8ff;
							WeightsPenalty = Math::Max<int64>(WeightsPenalty, 0);

							const int64 Diff = Sample - Rebuilt;
							Error += static_cast<uint64>(Diff * Diff + WeightsPenalty * WeightsPenalty);
							if (Error >= BestError)
							{
								break;
							}

							Trial.Update(Rebuilt, Dequant);
							Slice = (Slice << 3) | static_cast<uint64>(Code);
						}

						if (Error < BestError)
						{
							BestError = Error;
							BestSlice = Slice;
							BestLms   = Trial;
						}
					}

					Lms[Channel] = BestLms;
					BestSlice <<= (kSliceLen - SliceLen) * 3;
					WriteU64(Out, BestSlice);
				}
			}
		}
		return true;
	}

	bool FQoaReader::Open(const void* Data, size_t InSize)
	{
		using namespace QoaInternal;

		Bytes = nullptr;
		Info  = FAudioInfo();
		CachedFrame = ~0ull;
		if (!Qoa::IsQoa(Data, InSize) || InSize < kFileHeaderSize + 8)
		{
			return false;
		}

		const uint8* P = static_cast<const uint8*>(Data);
		const uint64 FirstFrame = ReadU64(P + kFileHeaderSize);
		const uint32 Channels   = static_cast<uint32>(FirstFrame >> 56);
		const uint32 Rate       = static_cast<uint32>(FirstFrame >> 32) & 0xFFFFFF;
		if (Channels == 0 || Channels > Qoa::kMaxChannels || Rate == 0)
		{
			return false;
		}

		Bytes = P;
		Size  = InSize;
		Info.NumFrames   = ReadU64(P) & 0xFFFFFFFFull;
		Info.NumChannels = Channels;
		Info.SampleRate  = Rate;
		return true;
	}

	bool FQoaReader::DecodeFrame(uint64 FrameIndex) const
	{
		using namespace QoaInternal;

		const uint32 Channels = Info.NumChannels;
		const uint64 Offset   = kFileHeaderSize + FrameIndex * FullFrameSize(Channels);
		if (Offset + 8 + 16ull * Channels > Size)
		{
			return false;
		}

		const uint8* P = Bytes + Offset;
		const uint64 Header   = ReadU64(P);
		const uint32 FrameLen = static_cast<uint32>(Header >> 16) & 0xFFFF;
		const uint64 FrameSize = Header & 0xFFFF;
		if (static_cast<uint32>(Header >> 56) != Channels || FrameLen == 0 || FrameLen > Qoa::kFrameLen || Offset + FrameSize > Size)
		{
			return false;
		}
		P += 8;

		FLms Lms[Qoa::kMaxChannels];
		for (uint32 Channel = 0; Channel < Channels; ++Channel)
		{
			uint64 History = ReadU64(P);
			uint64 Weights = ReadU64(P + 8);
			P += 16;
			for (int32 i = 0; i < 4; ++i)
			{
				Lms[Channel].History[i] = static_cast<int16>(History >> 48);
				Lms[Channel].Weights[i] = static_cast<int16>(Weights >> 48);
				History <<= 16;
				Weights <<= 16;
			}
		}

		Cache.SetNum(IntCastChecked<int32>(static_cast<uint64>(FrameLen) * Channels));
		constexpr float kInvScale = 1.0f / 32768.0f;
		for (uint32 SliceStart = 0; SliceStart < FrameLen; SliceStart += Qoa::kSliceLen)
		{
			const uint32 SliceEnd = Math::Min(SliceStart + Qoa::kSliceLen, FrameLen);
			for (uint32 Channel = 0; Channel < Channels; ++Channel)
			{
				uint64 Slice = ReadU64(P);
				P += 8;
				const int32 Scale = static_cast<int32>(Slice >> 60);
				Slice <<= 4;
				for (uint32 i = SliceStart; i < SliceEnd; ++i)
				{
					const int32 Predicted = Lms[Channel].Predict();
					const int32 Dequant   = kTables.Dequant[Scale][Slice >> 61];
					const int32 Rebuilt   = ClampS16(Predicted + Dequant);
					Lms[Channel].Update(Rebuilt, Dequant);
					Cache[i * Channels + Channel] = static_cast<float>(Rebuilt) * kInvScale;
					Slice <<= 3;
				}
			}
		}

		CachedFrame   = FrameIndex;
		CachedSamples = FrameLen;
		return true;
	}

	uint64 FQoaReader::ReadFrames(uint64 FrameOffset, uint64 NumFrames, float* Out) const
	{
		if (!IsOpen() || Out == nullptr)
		{
			return 0;
		}

		const uint32 Channels = Info.NumChannels;
		uint64 Written = 0;
		while (Written < NumFrames && FrameOffset + Written < Info.NumFrames)
		{
			const uint64 Position   = FrameOffset + Written;
			const uint64 FrameIndex = Position / Qoa::kFrameLen;
			if (FrameIndex != CachedFrame && !DecodeFrame(FrameIndex))
			{
				break;
			}

			const uint64 Within = Position - FrameIndex * Qoa::kFrameLen;
			if (Within >= CachedSamples)
			{
				break;
			}
			const uint64 Count = Math::Min<uint64>(NumFrames - Written, CachedSamples - Within);
			Memory::Memcpy(Out + Written * Channels, Cache.GetData() + Within * Channels, static_cast<size_t>(Count * Channels) * sizeof(float));
			Written += Count;
		}
		return Written;
	}
}
