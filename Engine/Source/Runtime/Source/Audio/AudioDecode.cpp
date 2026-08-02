#include "RuntimePCH.h"
#include "AudioDecode.h"

#include "MiniAudio/miniaudio.h"

namespace Lumina::Audio
{
	bool Probe(const void* Data, size_t Size, FAudioInfo& OutInfo)
	{
		ma_decoder Decoder;
		if (ma_decoder_init_memory(Data, Size, nullptr, &Decoder) != MA_SUCCESS)
		{
			return false;
		}

		ma_format Format;
		ma_uint32 Channels = 0;
		ma_uint32 SampleRate = 0;
		ma_uint64 Frames = 0;
		const bool bOk =
			ma_decoder_get_data_format(&Decoder, &Format, &Channels, &SampleRate, nullptr, 0) == MA_SUCCESS &&
			ma_decoder_get_length_in_pcm_frames(&Decoder, &Frames) == MA_SUCCESS;

		ma_decoder_uninit(&Decoder);

		if (!bOk || Channels == 0 || SampleRate == 0)
		{
			return false;
		}

		OutInfo.SampleRate  = SampleRate;
		OutInfo.NumChannels = Channels;
		OutInfo.NumFrames   = Frames;
		return true;
	}

	bool DecodePCM(const void* Data, size_t Size, FAudioInfo& OutInfo, TVector<float>& OutSamples)
	{
		ma_decoder_config Config = ma_decoder_config_init(ma_format_f32, 0, 0);
		ma_decoder Decoder;
		if (ma_decoder_init_memory(Data, Size, &Config, &Decoder) != MA_SUCCESS)
		{
			return false;
		}

		ma_format Format;
		ma_uint32 Channels = 0;
		ma_uint32 SampleRate = 0;
		ma_uint64 Frames = 0;
		if (ma_decoder_get_data_format(&Decoder, &Format, &Channels, &SampleRate, nullptr, 0) != MA_SUCCESS ||
			ma_decoder_get_length_in_pcm_frames(&Decoder, &Frames) != MA_SUCCESS ||
			Channels == 0 || SampleRate == 0)
		{
			ma_decoder_uninit(&Decoder);
			return false;
		}

		OutSamples.clear();
		ma_uint64 TotalRead = 0;

		// Frames can be 0 for streams that don't report length; read until EOF either way.
		if (Frames != 0)
		{
			OutSamples.resize((size_t)Frames * Channels);
			ma_decoder_read_pcm_frames(&Decoder, OutSamples.data(), Frames, &TotalRead);
			OutSamples.resize((size_t)TotalRead * Channels);
		}
		else
		{
			constexpr ma_uint64 ChunkFrames = 65536;
			TVector<float> Chunk;
			Chunk.resize((size_t)ChunkFrames * Channels);
			for (;;)
			{
				ma_uint64 Read = 0;
				ma_decoder_read_pcm_frames(&Decoder, Chunk.data(), ChunkFrames, &Read);
				if (Read == 0)
				{
					break;
				}
				OutSamples.insert(OutSamples.end(), Chunk.begin(), Chunk.begin() + (size_t)Read * Channels);
				TotalRead += Read;
			}
		}

		ma_decoder_uninit(&Decoder);

		OutInfo.SampleRate  = SampleRate;
		OutInfo.NumChannels = Channels;
		OutInfo.NumFrames   = TotalRead;
		return TotalRead != 0;
	}
}
