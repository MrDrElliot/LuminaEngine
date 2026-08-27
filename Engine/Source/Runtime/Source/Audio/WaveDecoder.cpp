#include "RuntimePCH.h"
#include "WaveDecoder.h"

#include "Core/Math/Scalar.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"

namespace Lumina::Audio
{
	namespace
	{
		constexpr uint16 kFormatPCM        = 0x0001;
		constexpr uint16 kFormatMSADPCM    = 0x0002;
		constexpr uint16 kFormatIEEEFloat  = 0x0003;
		constexpr uint16 kFormatALaw       = 0x0006;
		constexpr uint16 kFormatMuLaw      = 0x0007;
		constexpr uint16 kFormatIMAADPCM   = 0x0011;
		constexpr uint16 kFormatExtensible = 0xFFFE;

		constexpr size_t kRiffHeaderSize         = 12;
		constexpr size_t kChunkHeaderSize        = 8;
		constexpr size_t kMinFmtChunkSize        = 16;
		constexpr size_t kExtensibleFmtChunkSize = 40;

		uint16 ReadU16(const uint8* At)
		{
			uint16 Value = 0;
			Memory::Memcpy(&Value, At, sizeof(Value));
			return Value;
		}

		uint32 ReadU32(const uint8* At)
		{
			uint32 Value = 0;
			Memory::Memcpy(&Value, At, sizeof(Value));
			return Value;
		}

		bool ChunkIdEquals(const uint8* At, const char (&Id)[5])
		{
			return At[0] == (uint8)Id[0] && At[1] == (uint8)Id[1] && At[2] == (uint8)Id[2] && At[3] == (uint8)Id[3];
		}

		struct FWaveFormatChunk
		{
			uint16 FormatTag     = 0;
			uint16 Channels      = 0;
			uint32 SampleRate    = 0;
			uint16 BitsPerSample = 0;
		};

		const char* FormatTagName(uint16 Tag)
		{
			switch (Tag)
			{
			case kFormatPCM:       return "PCM";
			case kFormatMSADPCM:   return "Microsoft ADPCM";
			case kFormatIEEEFloat: return "IEEE float";
			case kFormatALaw:      return "A-law";
			case kFormatMuLaw:     return "mu-law";
			case kFormatIMAADPCM:  return "IMA ADPCM";
			}
			return "unknown";
		}

		bool ParseFormatChunk(const uint8* At, size_t Size, FWaveFormatChunk& Out)
		{
			if (Size < kMinFmtChunkSize)
			{
				return false;
			}

			Out.FormatTag     = ReadU16(At + 0);
			Out.Channels      = ReadU16(At + 2);
			Out.SampleRate    = ReadU32(At + 4);
			Out.BitsPerSample = ReadU16(At + 14);

			// The real tag of an extensible file is the leading field of the SubFormat GUID.
			if (Out.FormatTag == kFormatExtensible)
			{
				if (Size < kExtensibleFmtChunkSize || ReadU16(At + 16) < 22)
				{
					return false;
				}
				Out.FormatTag = ReadU16(At + 24);
			}

			return Out.Channels != 0 && Out.SampleRate != 0 && Out.BitsPerSample != 0;
		}

		EWaveSampleFormat ResolveSampleFormat(uint16 Tag, uint16 BitsPerSample)
		{
			switch (Tag)
			{
			case kFormatPCM:
				switch (BitsPerSample)
				{
				case 8:  return EWaveSampleFormat::PCM8;
				case 16: return EWaveSampleFormat::PCM16;
				case 24: return EWaveSampleFormat::PCM24;
				case 32: return EWaveSampleFormat::PCM32;
				}
				break;

			case kFormatIEEEFloat:
				switch (BitsPerSample)
				{
				case 32: return EWaveSampleFormat::Float32;
				case 64: return EWaveSampleFormat::Float64;
				}
				break;

			case kFormatALaw:
				if (BitsPerSample == 8)
				{
					return EWaveSampleFormat::ALaw;
				}
				break;

			case kFormatMuLaw:
				if (BitsPerSample == 8)
				{
					return EWaveSampleFormat::MuLaw;
				}
				break;
			}

			return EWaveSampleFormat::Unknown;
		}

		uint32 BitsPerSampleOf(EWaveSampleFormat Format)
		{
			switch (Format)
			{
			case EWaveSampleFormat::PCM8:
			case EWaveSampleFormat::ALaw:
			case EWaveSampleFormat::MuLaw:   return 8;
			case EWaveSampleFormat::PCM16:   return 16;
			case EWaveSampleFormat::PCM24:   return 24;
			case EWaveSampleFormat::PCM32:
			case EWaveSampleFormat::Float32: return 32;
			case EWaveSampleFormat::Float64: return 64;
			default:                         return 0;
			}
		}

		// G.711 expansion back to the 16 bit linear value the sample was companded from.
		int32 ExpandALaw(uint8 Value)
		{
			Value ^= 0x55;

			const int32 Segment = (Value & 0x70) >> 4;
			int32 Magnitude = (Value & 0x0F) << 4;

			if (Segment == 0)
			{
				Magnitude += 8;
			}
			else if (Segment == 1)
			{
				Magnitude += 0x108;
			}
			else
			{
				Magnitude = (Magnitude + 0x108) << (Segment - 1);
			}

			return (Value & 0x80) ? Magnitude : -Magnitude;
		}

		int32 ExpandMuLaw(uint8 Value)
		{
			Value = (uint8)~Value;

			const int32 Magnitude = (((Value & 0x0F) << 3) + 0x84) << ((Value & 0x70) >> 4);
			return (Value & 0x80) ? (0x84 - Magnitude) : (Magnitude - 0x84);
		}
	}

	const char* ToString(EWaveSampleFormat Format)
	{
		switch (Format)
		{
		case EWaveSampleFormat::PCM8:    return "PCM8";
		case EWaveSampleFormat::PCM16:   return "PCM16";
		case EWaveSampleFormat::PCM24:   return "PCM24";
		case EWaveSampleFormat::PCM32:   return "PCM32";
		case EWaveSampleFormat::Float32: return "Float32";
		case EWaveSampleFormat::Float64: return "Float64";
		case EWaveSampleFormat::ALaw:    return "ALaw";
		case EWaveSampleFormat::MuLaw:   return "MuLaw";
		default:                         return "Unknown";
		}
	}

	bool FWaveReader::Open(const void* Data, size_t Size)
	{
		*this = FWaveReader();

		const uint8* Bytes = (const uint8*)Data;
		if (Bytes == nullptr || Size < kRiffHeaderSize)
		{
			return false;
		}

		if (!ChunkIdEquals(Bytes, "RIFF") || !ChunkIdEquals(Bytes + 8, "WAVE"))
		{
			return false;
		}

		FWaveFormatChunk Format;
		bool bHaveFormat = false;

		size_t Offset = kRiffHeaderSize;
		while (Offset + kChunkHeaderSize <= Size)
		{
			const uint8* Id = Bytes + Offset;
			const uint32 DeclaredSize = ReadU32(Bytes + Offset + 4);

			const size_t Payload   = Offset + kChunkHeaderSize;
			const size_t ChunkSize = Math::Min((size_t)DeclaredSize, Size - Payload);

			if (ChunkIdEquals(Id, "fmt "))
			{
				if (!ParseFormatChunk(Bytes + Payload, ChunkSize, Format))
				{
					return false;
				}
				bHaveFormat = true;
			}
			else if (ChunkIdEquals(Id, "data"))
			{
				if (!bHaveFormat)
				{
					return false;
				}

				const EWaveSampleFormat Resolved = ResolveSampleFormat(Format.FormatTag, Format.BitsPerSample);
				if (Resolved == EWaveSampleFormat::Unknown)
				{
					LOG_WARN("[Wave] unsupported format '{0}' at {1} bits per sample", FormatTagName(Format.FormatTag), Format.BitsPerSample);
					return false;
				}

				// Derived rather than trusting nBlockAlign, which encoders get wrong often enough to matter.
				const uint32 FrameStride = Format.Channels * (BitsPerSampleOf(Resolved) / 8);

				SampleFormat     = Resolved;
				Samples          = Bytes + Payload;
				BytesPerFrame    = FrameStride;
				Info.SampleRate  = Format.SampleRate;
				Info.NumChannels = Format.Channels;
				Info.NumFrames   = ChunkSize / FrameStride;
				return true;
			}

			Offset = Payload + ChunkSize + (DeclaredSize & 1);
		}

		return false;
	}

	uint64 FWaveReader::ReadFrames(uint64 FrameOffset, uint64 NumFrames, float* Out) const
	{
		if (!IsOpen() || Out == nullptr || FrameOffset >= Info.NumFrames)
		{
			return 0;
		}

		const uint64 FrameCount = Math::Min(NumFrames, Info.NumFrames - FrameOffset);
		if (FrameCount == 0)
		{
			return 0;
		}

		const uint8* Src   = Samples + FrameOffset * BytesPerFrame;
		const size_t Count = (size_t)(FrameCount * Info.NumChannels);

		switch (SampleFormat)
		{
		// Biased around 128 so silence decodes to exact zero, unlike dr_wav which leaves a 1/255 offset.
		case EWaveSampleFormat::PCM8:
			for (size_t i = 0; i < Count; ++i)
			{
				Out[i] = ((float)Src[i] - 128.0f) * (1.0f / 128.0f);
			}
			break;

		case EWaveSampleFormat::PCM16:
			for (size_t i = 0; i < Count; ++i)
			{
				Out[i] = (float)(int16)ReadU16(Src + i * 2) * (1.0f / 32768.0f);
			}
			break;

		case EWaveSampleFormat::PCM24:
			for (size_t i = 0; i < Count; ++i)
			{
				const uint8* At = Src + i * 3;
				const int32 Value = (int32)(((uint32)At[0] << 8) | ((uint32)At[1] << 16) | ((uint32)At[2] << 24)) >> 8;
				Out[i] = (float)Value * (1.0f / 8388608.0f);
			}
			break;

		case EWaveSampleFormat::PCM32:
			for (size_t i = 0; i < Count; ++i)
			{
				Out[i] = (float)(int32)ReadU32(Src + i * 4) * (1.0f / 2147483648.0f);
			}
			break;

		case EWaveSampleFormat::Float32:
			Memory::Memcpy(Out, Src, Count * sizeof(float));
			break;

		case EWaveSampleFormat::Float64:
			for (size_t i = 0; i < Count; ++i)
			{
				double Value = 0.0;
				Memory::Memcpy(&Value, Src + i * 8, sizeof(Value));
				Out[i] = (float)Value;
			}
			break;

		case EWaveSampleFormat::ALaw:
			for (size_t i = 0; i < Count; ++i)
			{
				Out[i] = (float)ExpandALaw(Src[i]) * (1.0f / 32768.0f);
			}
			break;

		case EWaveSampleFormat::MuLaw:
			for (size_t i = 0; i < Count; ++i)
			{
				Out[i] = (float)ExpandMuLaw(Src[i]) * (1.0f / 32768.0f);
			}
			break;

		default:
			return 0;
		}

		return FrameCount;
	}
}
