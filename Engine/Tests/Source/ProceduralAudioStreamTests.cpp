#include <gtest/gtest.h>

#include "Audio/ProceduralAudioStream.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"

using namespace Lumina;

namespace
{
    TVector<float> Ramp(uint32 Frames, uint32 Channels, float Start)
    {
        TVector<float> Out((size_t)Frames * Channels);
        for (size_t i = 0; i < Out.size(); ++i)
        {
            Out[i] = Start + (float)i;
        }
        return Out;
    }
}

TEST(ProceduralAudioStream, RejectsInvalidParameters)
{
    EXPECT_FALSE(FProceduralAudioStream(0, 2, 128).IsValid());
    EXPECT_FALSE(FProceduralAudioStream(48000, 0, 128).IsValid());
    EXPECT_FALSE(FProceduralAudioStream(48000, 2, 0).IsValid());

    FProceduralAudioStream Valid(48000, 2, 128);
    EXPECT_TRUE(Valid.IsValid());
    EXPECT_EQ(Valid.GetSampleRate(), 48000u);
    EXPECT_EQ(Valid.GetChannelCount(), 2u);
}

TEST(ProceduralAudioStream, ReadOnAnInvalidStreamIsSilentNotUndefined)
{
    FProceduralAudioStream Stream(0, 2, 128);
    ASSERT_FALSE(Stream.IsValid());

    float Block[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    EXPECT_EQ(Stream.Read(Block, 4), 0u);
    for (float Sample : Block)
    {
        EXPECT_FLOAT_EQ(Sample, 0.0f);
    }
}

TEST(ProceduralAudioStream, RoundTripsInterleavedFrames)
{
    FProceduralAudioStream Stream(48000, 2, 64);

    EXPECT_EQ(Stream.GetAvailableWriteFrames(), 64u);
    EXPECT_EQ(Stream.GetAvailableReadFrames(), 0u);

    const TVector<float> Source = Ramp(10, 2, 1.0f);
    EXPECT_EQ(Stream.Write(Source.data(), 10), 10u);

    EXPECT_EQ(Stream.GetAvailableReadFrames(), 10u);
    EXPECT_EQ(Stream.GetAvailableWriteFrames(), 54u);

    TVector<float> Out(20, -1.0f);
    EXPECT_EQ(Stream.Read(Out.data(), 10), 10u);

    for (size_t i = 0; i < Out.size(); ++i)
    {
        EXPECT_FLOAT_EQ(Out[i], Source[i]) << "sample " << i;
    }

    EXPECT_EQ(Stream.GetAvailableReadFrames(), 0u);
}

TEST(ProceduralAudioStream, SaturatesWhenFull)
{
    FProceduralAudioStream Stream(48000, 1, 8);

    const TVector<float> Source = Ramp(16, 1, 0.0f);

    // Only the capacity is accepted; the rest is dropped rather than overwriting unread frames.
    EXPECT_EQ(Stream.Write(Source.data(), 16), 8u);
    EXPECT_EQ(Stream.GetAvailableWriteFrames(), 0u);
    EXPECT_EQ(Stream.Write(Source.data(), 1), 0u);

    TVector<float> Out(8, -1.0f);
    EXPECT_EQ(Stream.Read(Out.data(), 8), 8u);
    for (uint32 i = 0; i < 8; ++i)
    {
        EXPECT_FLOAT_EQ(Out[i], (float)i);
    }
}

TEST(ProceduralAudioStream, UnderrunPadsWithSilence)
{
    FProceduralAudioStream Stream(48000, 2, 16);

    const TVector<float> Source = Ramp(3, 2, 5.0f);
    ASSERT_EQ(Stream.Write(Source.data(), 3), 3u);

    TVector<float> Out(16, -1.0f);
    EXPECT_EQ(Stream.Read(Out.data(), 8), 3u);

    for (size_t i = 0; i < 6; ++i)
    {
        EXPECT_FLOAT_EQ(Out[i], Source[i]) << "sample " << i;
    }

    // Everything past the real data is zeroed, so an underrun never plays stale samples.
    for (size_t i = 6; i < 16; ++i)
    {
        EXPECT_FLOAT_EQ(Out[i], 0.0f) << "sample " << i;
    }
}

TEST(ProceduralAudioStream, WrapsAroundTheRing)
{
    constexpr uint32 Capacity = 8;
    FProceduralAudioStream Stream(48000, 2, Capacity);

    // Advance the cursors so the next write straddles the end of the storage.
    const TVector<float> Priming = Ramp(6, 2, 0.0f);
    ASSERT_EQ(Stream.Write(Priming.data(), 6), 6u);

    TVector<float> Drain(12, 0.0f);
    ASSERT_EQ(Stream.Read(Drain.data(), 6), 6u);

    const TVector<float> Source = Ramp(7, 2, 100.0f);
    ASSERT_EQ(Stream.Write(Source.data(), 7), 7u);
    EXPECT_EQ(Stream.GetAvailableReadFrames(), 7u);

    TVector<float> Out(14, -1.0f);
    ASSERT_EQ(Stream.Read(Out.data(), 7), 7u);

    for (size_t i = 0; i < Out.size(); ++i)
    {
        EXPECT_FLOAT_EQ(Out[i], Source[i]) << "sample " << i;
    }
}

TEST(ProceduralAudioStream, PartialReadsAndWritesStayAligned)
{
    FProceduralAudioStream Stream(48000, 3, 32);

    float Next = 0.0f;
    float Expected = 0.0f;

    // Deliberately mismatched chunk sizes, so the read and write cursors wrap at different points.
    for (uint32 Round = 0; Round < 50; ++Round)
    {
        const uint32 WriteFrames = 5;
        TVector<float> Chunk((size_t)WriteFrames * 3);
        for (float& Sample : Chunk)
        {
            Sample = Next++;
        }
        ASSERT_EQ(Stream.Write(Chunk.data(), WriteFrames), WriteFrames) << "round " << Round;

        const uint32 ReadFrames = 5;
        TVector<float> Out((size_t)ReadFrames * 3, -1.0f);
        ASSERT_EQ(Stream.Read(Out.data(), ReadFrames), ReadFrames) << "round " << Round;

        for (float Sample : Out)
        {
            ASSERT_FLOAT_EQ(Sample, Expected++) << "round " << Round;
        }
    }
}

TEST(ProceduralAudioStream, SingleProducerSingleConsumerStaysOrdered)
{
    constexpr uint32 kTotalFrames = 200000;
    constexpr uint32 kChunk = 64;

    FProceduralAudioStream Stream(48000, 1, 512);

    TAtomic<bool> bProducerDone{false};

    FThread Producer([&]()
    {
        float Next = 0.0f;
        uint32 Written = 0;
        TVector<float> Chunk(kChunk);

        while (Written < kTotalFrames)
        {
            const uint32 Want = Math::Min(kChunk, kTotalFrames - Written);
            for (uint32 i = 0; i < Want; ++i)
            {
                Chunk[i] = Next + (float)i;
            }

            const uint32 Wrote = Stream.Write(Chunk.data(), Want);
            Next    += (float)Wrote;
            Written += Wrote;
        }

        bProducerDone.store(true, Atomic::MemoryOrderRelease);
    });

    float ExpectedNext = 0.0f;
    uint32 TotalRead = 0;
    TVector<float> Out(kChunk);

    while (TotalRead < kTotalFrames)
    {
        const uint32 Read = Stream.Read(Out.data(), kChunk);
        for (uint32 i = 0; i < Read; ++i)
        {
            ASSERT_FLOAT_EQ(Out[i], ExpectedNext) << "frame " << TotalRead + i;
            ExpectedNext += 1.0f;
        }
        TotalRead += Read;
    }

    Producer.Join();

    EXPECT_TRUE(bProducerDone.load(Atomic::MemoryOrderAcquire));
    EXPECT_EQ(TotalRead, kTotalFrames);
    EXPECT_FLOAT_EQ(ExpectedNext, (float)kTotalFrames);
}
