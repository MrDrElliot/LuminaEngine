#include <gtest/gtest.h>

#include "Audio/AudioDecode.h"
#include "Audio/WaveDecoder.h"
#include "Containers/Vector.h"

using namespace Lumina;
using namespace Lumina::Audio;

namespace
{
    constexpr uint16 kTagPCM        = 0x0001;
    constexpr uint16 kTagMSADPCM    = 0x0002;
    constexpr uint16 kTagIEEEFloat  = 0x0003;
    constexpr uint16 kTagALaw       = 0x0006;
    constexpr uint16 kTagMuLaw      = 0x0007;
    constexpr uint16 kTagExtensible = 0xFFFE;

    struct FWaveBuilder
    {
        TVector<uint8> Bytes;

        void PushU16(uint16 Value)
        {
            Bytes.push_back((uint8)(Value & 0xFF));
            Bytes.push_back((uint8)(Value >> 8));
        }

        void PushU32(uint32 Value)
        {
            Bytes.push_back((uint8)(Value & 0xFF));
            Bytes.push_back((uint8)((Value >> 8) & 0xFF));
            Bytes.push_back((uint8)((Value >> 16) & 0xFF));
            Bytes.push_back((uint8)((Value >> 24) & 0xFF));
        }

        void PushId(const char* Id)
        {
            for (uint32 i = 0; i < 4; ++i)
            {
                Bytes.push_back((uint8)Id[i]);
            }
        }

        void PushRaw(const void* Data, size_t Size)
        {
            const uint8* At = (const uint8*)Data;
            Bytes.insert(Bytes.end(), At, At + Size);
        }

        void BeginRiff()
        {
            PushId("RIFF");
            PushU32(0);
            PushId("WAVE");
        }

        // Patches the RIFF size field now that every chunk has been appended.
        void FinishRiff()
        {
            const uint32 Size = (uint32)(Bytes.size() - 8);
            Bytes[4] = (uint8)(Size & 0xFF);
            Bytes[5] = (uint8)((Size >> 8) & 0xFF);
            Bytes[6] = (uint8)((Size >> 16) & 0xFF);
            Bytes[7] = (uint8)((Size >> 24) & 0xFF);
        }

        void PushFmt(uint16 Tag, uint16 Channels, uint32 SampleRate, uint16 Bits)
        {
            const uint16 BlockAlign = (uint16)(Channels * (Bits / 8));

            PushId("fmt ");
            PushU32(16);
            PushU16(Tag);
            PushU16(Channels);
            PushU32(SampleRate);
            PushU32(SampleRate * BlockAlign);
            PushU16(BlockAlign);
            PushU16(Bits);
        }

        void PushExtensibleFmt(uint16 SubTag, uint16 Channels, uint32 SampleRate, uint16 Bits)
        {
            const uint16 BlockAlign = (uint16)(Channels * (Bits / 8));

            PushId("fmt ");
            PushU32(40);
            PushU16(kTagExtensible);
            PushU16(Channels);
            PushU32(SampleRate);
            PushU32(SampleRate * BlockAlign);
            PushU16(BlockAlign);
            PushU16(Bits);
            PushU16(22);
            PushU16(Bits);
            PushU32(0x3);
            PushU16(SubTag);
            PushU16(0x0000);
            PushU32(0x00100000);
            PushU32(0xAA000080);
            PushU32(0x719B3800);
        }

        void PushData(const void* Data, size_t Size)
        {
            PushId("data");
            PushU32((uint32)Size);
            PushRaw(Data, Size);
            if (Size & 1)
            {
                Bytes.push_back(0);
            }
        }

        void PushChunk(const char* Id, const void* Data, size_t Size)
        {
            PushId(Id);
            PushU32((uint32)Size);
            PushRaw(Data, Size);
            if (Size & 1)
            {
                Bytes.push_back(0);
            }
        }
    };

    TVector<uint8> MakeWave(uint16 Tag, uint16 Channels, uint32 SampleRate, uint16 Bits, const void* Data, size_t Size)
    {
        FWaveBuilder Builder;
        Builder.BeginRiff();
        Builder.PushFmt(Tag, Channels, SampleRate, Bits);
        Builder.PushData(Data, Size);
        Builder.FinishRiff();
        return Builder.Bytes;
    }

    TVector<float> ReadAll(const FWaveReader& Reader)
    {
        const FAudioInfo& Info = Reader.GetInfo();
        TVector<float> Samples((size_t)(Info.NumFrames * Info.NumChannels));
        const uint64 Read = Reader.ReadFrames(0, Info.NumFrames, Samples.data());
        EXPECT_EQ(Read, Info.NumFrames);
        return Samples;
    }
}

TEST(WaveDecoder, RejectsGarbage)
{
    FWaveReader Reader;

    EXPECT_FALSE(Reader.Open(nullptr, 0));
    EXPECT_FALSE(Reader.IsOpen());

    const uint8 TooShort[] = { 'R', 'I', 'F', 'F' };
    EXPECT_FALSE(Reader.Open(TooShort, sizeof(TooShort)));

    const uint8 NotRiff[16] = { 'J', 'U', 'N', 'K' };
    EXPECT_FALSE(Reader.Open(NotRiff, sizeof(NotRiff)));

    // A well formed RIFF header carrying no data chunk is not decodable.
    FWaveBuilder Builder;
    Builder.BeginRiff();
    Builder.PushFmt(kTagPCM, 1, 44100, 16);
    Builder.FinishRiff();
    EXPECT_FALSE(Reader.Open(Builder.Bytes.data(), Builder.Bytes.size()));
}

TEST(WaveDecoder, RejectsUnsupportedCodec)
{
    const uint8 Payload[8] = {};
    const TVector<uint8> File = MakeWave(kTagMSADPCM, 1, 44100, 4, Payload, sizeof(Payload));

    FWaveReader Reader;
    EXPECT_FALSE(Reader.Open(File.data(), File.size()));
}

TEST(WaveDecoder, DecodesPCM16Mono)
{
    const int16 Source[] = { 0, 32767, -32768, 16384 };
    const TVector<uint8> File = MakeWave(kTagPCM, 1, 44100, 16, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));

    EXPECT_EQ(Reader.GetSampleFormat(), EWaveSampleFormat::PCM16);
    EXPECT_EQ(Reader.GetInfo().SampleRate, 44100u);
    EXPECT_EQ(Reader.GetInfo().NumChannels, 1u);
    EXPECT_EQ(Reader.GetInfo().NumFrames, 4u);

    const TVector<float> Samples = ReadAll(Reader);
    ASSERT_EQ(Samples.size(), 4u);
    EXPECT_FLOAT_EQ(Samples[0], 0.0f);
    EXPECT_NEAR(Samples[1], 1.0f, 1.0f / 32768.0f);
    EXPECT_FLOAT_EQ(Samples[2], -1.0f);
    EXPECT_FLOAT_EQ(Samples[3], 0.5f);
}

TEST(WaveDecoder, DecodesPCM16StereoInterleaved)
{
    const int16 Source[] = { 1000, -1000, 2000, -2000, 3000, -3000 };
    const TVector<uint8> File = MakeWave(kTagPCM, 2, 48000, 16, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));

    EXPECT_EQ(Reader.GetInfo().NumChannels, 2u);
    EXPECT_EQ(Reader.GetInfo().NumFrames, 3u);

    const TVector<float> Samples = ReadAll(Reader);
    ASSERT_EQ(Samples.size(), 6u);
    for (uint32 Frame = 0; Frame < 3; ++Frame)
    {
        EXPECT_FLOAT_EQ(Samples[Frame * 2 + 0], -Samples[Frame * 2 + 1]);
    }
    EXPECT_FLOAT_EQ(Samples[0], 1000.0f / 32768.0f);
    EXPECT_FLOAT_EQ(Samples[5], -3000.0f / 32768.0f);
}

TEST(WaveDecoder, DecodesPCM8AsUnsigned)
{
    const uint8 Source[] = { 128, 255, 0, 192 };
    const TVector<uint8> File = MakeWave(kTagPCM, 1, 22050, 8, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));
    EXPECT_EQ(Reader.GetSampleFormat(), EWaveSampleFormat::PCM8);

    const TVector<float> Samples = ReadAll(Reader);
    ASSERT_EQ(Samples.size(), 4u);
    EXPECT_FLOAT_EQ(Samples[0], 0.0f);
    EXPECT_FLOAT_EQ(Samples[1], 127.0f / 128.0f);
    EXPECT_FLOAT_EQ(Samples[2], -1.0f);
    EXPECT_FLOAT_EQ(Samples[3], 0.5f);
}

TEST(WaveDecoder, DecodesPCM24WithSignExtension)
{
    const uint8 Source[] =
    {
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x80,
        0xFF, 0xFF, 0x7F,
        0x00, 0x00, 0x40,
    };
    const TVector<uint8> File = MakeWave(kTagPCM, 1, 96000, 24, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));
    EXPECT_EQ(Reader.GetSampleFormat(), EWaveSampleFormat::PCM24);
    EXPECT_EQ(Reader.GetInfo().NumFrames, 4u);

    const TVector<float> Samples = ReadAll(Reader);
    EXPECT_FLOAT_EQ(Samples[0], 0.0f);
    EXPECT_FLOAT_EQ(Samples[1], -1.0f);
    EXPECT_NEAR(Samples[2], 1.0f, 1.0f / 8388608.0f);
    EXPECT_FLOAT_EQ(Samples[3], 0.5f);
}

TEST(WaveDecoder, DecodesPCM32)
{
    const int32 Source[] = { 0, 1073741824, -2147483647 - 1 };
    const TVector<uint8> File = MakeWave(kTagPCM, 1, 44100, 32, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));
    EXPECT_EQ(Reader.GetSampleFormat(), EWaveSampleFormat::PCM32);

    const TVector<float> Samples = ReadAll(Reader);
    EXPECT_FLOAT_EQ(Samples[0], 0.0f);
    EXPECT_FLOAT_EQ(Samples[1], 0.5f);
    EXPECT_FLOAT_EQ(Samples[2], -1.0f);
}

TEST(WaveDecoder, DecodesFloat32Verbatim)
{
    const float Source[] = { 0.0f, 1.0f, -1.0f, 0.25f, -0.75f };
    const TVector<uint8> File = MakeWave(kTagIEEEFloat, 1, 48000, 32, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));
    EXPECT_EQ(Reader.GetSampleFormat(), EWaveSampleFormat::Float32);

    const TVector<float> Samples = ReadAll(Reader);
    ASSERT_EQ(Samples.size(), 5u);
    for (size_t i = 0; i < Samples.size(); ++i)
    {
        EXPECT_FLOAT_EQ(Samples[i], Source[i]);
    }
}

TEST(WaveDecoder, DecodesFloat64)
{
    const double Source[] = { 0.0, 0.5, -0.25 };
    const TVector<uint8> File = MakeWave(kTagIEEEFloat, 1, 48000, 64, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));
    EXPECT_EQ(Reader.GetSampleFormat(), EWaveSampleFormat::Float64);

    const TVector<float> Samples = ReadAll(Reader);
    EXPECT_FLOAT_EQ(Samples[0], 0.0f);
    EXPECT_FLOAT_EQ(Samples[1], 0.5f);
    EXPECT_FLOAT_EQ(Samples[2], -0.25f);
}

TEST(WaveDecoder, DecodesCompandedFormats)
{
    const uint8 MuLawSource[] = { 0xFF, 0x7F, 0x00 };
    const TVector<uint8> MuLawFile = MakeWave(kTagMuLaw, 1, 8000, 8, MuLawSource, sizeof(MuLawSource));

    FWaveReader MuLawReader;
    ASSERT_TRUE(MuLawReader.Open(MuLawFile.data(), MuLawFile.size()));
    EXPECT_EQ(MuLawReader.GetSampleFormat(), EWaveSampleFormat::MuLaw);

    const TVector<float> MuLawSamples = ReadAll(MuLawReader);
    EXPECT_FLOAT_EQ(MuLawSamples[0], 0.0f);
    EXPECT_FLOAT_EQ(MuLawSamples[1], 0.0f);
    EXPECT_FLOAT_EQ(MuLawSamples[2], -32124.0f / 32768.0f);

    const uint8 ALawSource[] = { 0xD5, 0x55 };
    const TVector<uint8> ALawFile = MakeWave(kTagALaw, 1, 8000, 8, ALawSource, sizeof(ALawSource));

    FWaveReader ALawReader;
    ASSERT_TRUE(ALawReader.Open(ALawFile.data(), ALawFile.size()));
    EXPECT_EQ(ALawReader.GetSampleFormat(), EWaveSampleFormat::ALaw);

    const TVector<float> ALawSamples = ReadAll(ALawReader);
    EXPECT_FLOAT_EQ(ALawSamples[0], 8.0f / 32768.0f);
    EXPECT_FLOAT_EQ(ALawSamples[1], -8.0f / 32768.0f);
}

TEST(WaveDecoder, ResolvesExtensibleSubFormat)
{
    const int16 Source[] = { 0, 16384, -16384, 0 };

    FWaveBuilder Builder;
    Builder.BeginRiff();
    Builder.PushExtensibleFmt(kTagPCM, 2, 48000, 16);
    Builder.PushData(Source, sizeof(Source));
    Builder.FinishRiff();

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(Builder.Bytes.data(), Builder.Bytes.size()));

    EXPECT_EQ(Reader.GetSampleFormat(), EWaveSampleFormat::PCM16);
    EXPECT_EQ(Reader.GetInfo().NumChannels, 2u);
    EXPECT_EQ(Reader.GetInfo().NumFrames, 2u);

    const TVector<float> Samples = ReadAll(Reader);
    EXPECT_FLOAT_EQ(Samples[1], 0.5f);
    EXPECT_FLOAT_EQ(Samples[2], -0.5f);
}

TEST(WaveDecoder, SkipsUnknownAndOddSizedChunks)
{
    const int16 Source[] = { 100, 200, 300, 400 };
    const uint8 Junk[] = { 'L', 'u', 'm', 'i', 'n', 'a', '!' };

    FWaveBuilder Builder;
    Builder.BeginRiff();
    Builder.PushChunk("LIST", Junk, sizeof(Junk));
    Builder.PushFmt(kTagPCM, 1, 44100, 16);
    Builder.PushChunk("fact", Junk, 3);
    Builder.PushData(Source, sizeof(Source));
    Builder.FinishRiff();

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(Builder.Bytes.data(), Builder.Bytes.size()));
    EXPECT_EQ(Reader.GetInfo().NumFrames, 4u);

    const TVector<float> Samples = ReadAll(Reader);
    EXPECT_FLOAT_EQ(Samples[0], 100.0f / 32768.0f);
    EXPECT_FLOAT_EQ(Samples[3], 400.0f / 32768.0f);
}

TEST(WaveDecoder, ClampsTruncatedDataChunk)
{
    const int16 Source[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    TVector<uint8> File = MakeWave(kTagPCM, 1, 44100, 16, Source, sizeof(Source));

    // Lop off half the samples while leaving the declared data size intact.
    File.resize(File.size() - 8);

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));
    EXPECT_EQ(Reader.GetInfo().NumFrames, 4u);

    const TVector<float> Samples = ReadAll(Reader);
    ASSERT_EQ(Samples.size(), 4u);
    EXPECT_FLOAT_EQ(Samples[3], 4.0f / 32768.0f);
}

TEST(WaveDecoder, ReadsFromAnOffsetAndStopsAtEnd)
{
    const int16 Source[] = { 0, 100, 200, 300, 400, 500 };
    const TVector<uint8> File = MakeWave(kTagPCM, 1, 44100, 16, Source, sizeof(Source));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));

    float Block[4] = {};
    EXPECT_EQ(Reader.ReadFrames(2, 4, Block), 4u);
    EXPECT_FLOAT_EQ(Block[0], 200.0f / 32768.0f);
    EXPECT_FLOAT_EQ(Block[3], 500.0f / 32768.0f);

    // A read straddling the end returns only what is left.
    EXPECT_EQ(Reader.ReadFrames(4, 4, Block), 2u);
    EXPECT_FLOAT_EQ(Block[0], 400.0f / 32768.0f);
    EXPECT_FLOAT_EQ(Block[1], 500.0f / 32768.0f);

    EXPECT_EQ(Reader.ReadFrames(6, 4, Block), 0u);
    EXPECT_EQ(Reader.ReadFrames(0, 4, nullptr), 0u);
}

TEST(WaveDecoder, ChunkedReadsMatchOneShotRead)
{
    TVector<int16> Source(1000);
    for (size_t i = 0; i < Source.size(); ++i)
    {
        Source[i] = (int16)((i * 37) % 30000 - 15000);
    }

    const TVector<uint8> File = MakeWave(kTagPCM, 2, 48000, 16, Source.data(), Source.size() * sizeof(int16));

    FWaveReader Reader;
    ASSERT_TRUE(Reader.Open(File.data(), File.size()));
    ASSERT_EQ(Reader.GetInfo().NumFrames, 500u);

    const TVector<float> OneShot = ReadAll(Reader);

    TVector<float> Chunked(OneShot.size());
    uint64 Frame = 0;
    while (Frame < Reader.GetInfo().NumFrames)
    {
        const uint64 Read = Reader.ReadFrames(Frame, 37, Chunked.data() + Frame * 2);
        ASSERT_NE(Read, 0u);
        Frame += Read;
    }

    EXPECT_EQ(Frame, 500u);
    for (size_t i = 0; i < OneShot.size(); ++i)
    {
        ASSERT_FLOAT_EQ(Chunked[i], OneShot[i]) << "sample " << i;
    }
}

TEST(WaveDecoder, ProbeMatchesDecode)
{
    const int16 Source[] = { 0, 1000, -1000, 2000, -2000, 3000 };
    const TVector<uint8> File = MakeWave(kTagPCM, 3, 32000, 16, Source, sizeof(Source));

    FAudioInfo Probed;
    ASSERT_TRUE(Probe(File.data(), File.size(), Probed));
    EXPECT_EQ(Probed.SampleRate, 32000u);
    EXPECT_EQ(Probed.NumChannels, 3u);
    EXPECT_EQ(Probed.NumFrames, 2u);
    EXPECT_DOUBLE_EQ(Probed.GetDuration(), 2.0 / 32000.0);

    FAudioInfo Decoded;
    TVector<float> Samples;
    ASSERT_TRUE(DecodePCM(File.data(), File.size(), Decoded, Samples));

    EXPECT_EQ(Decoded.SampleRate, Probed.SampleRate);
    EXPECT_EQ(Decoded.NumChannels, Probed.NumChannels);
    EXPECT_EQ(Decoded.NumFrames, Probed.NumFrames);
    ASSERT_EQ(Samples.size(), 6u);
    EXPECT_FLOAT_EQ(Samples[1], 1000.0f / 32768.0f);
}

TEST(WaveDecoder, DecodeRejectsEmptyAndInvalidInput)
{
    FAudioInfo Info;
    TVector<float> Samples;

    EXPECT_FALSE(DecodePCM(nullptr, 0, Info, Samples));

    const uint8 Empty[2] = {};
    const TVector<uint8> NoFrames = MakeWave(kTagPCM, 1, 44100, 16, Empty, 0);
    EXPECT_FALSE(DecodePCM(NoFrames.data(), NoFrames.size(), Info, Samples));

    // Probe still reports the format of a zero length clip.
    EXPECT_TRUE(Probe(NoFrames.data(), NoFrames.size(), Info));
    EXPECT_EQ(Info.NumFrames, 0u);
    EXPECT_EQ(Info.SampleRate, 44100u);
}
