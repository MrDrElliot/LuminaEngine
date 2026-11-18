#include <gtest/gtest.h>

#include "Audio/AudioDecode.h"
#include "Audio/AudioReader.h"
#include "Audio/QoaCodec.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include <cmath>

using namespace Lumina;
using namespace Lumina::Audio;

namespace
{
    constexpr float  kTwoPi    = 6.28318531f;
    constexpr uint32 kRate     = 48000;
    constexpr uint32 kChannels = 2;

    // Past two whole frames, so the last frame is partial and a read crosses frame boundaries.
    constexpr uint64 kFrames = Qoa::kFrameLen * 2 + 1234;

    TVector<float> MakeTone()
    {
        TVector<float> Samples(IntCastChecked<int32>(kFrames * kChannels));
        for (int32 Frame = 0; Frame < (int32)kFrames; ++Frame)
        {
            const float Time = (float)Frame / (float)kRate;
            Samples[Frame * kChannels + 0] = 0.5f * std::sin(kTwoPi * 440.0f * Time);
            Samples[Frame * kChannels + 1] = 0.3f * std::sin(kTwoPi * 660.0f * Time) + 0.1f * std::sin(kTwoPi * 97.0f * Time);
        }
        return Samples;
    }
}

TEST(QoaCodec, RoundTripsWithinAFewPercentAtAFifthOfPcmSize)
{
    const TVector<float> Source = MakeTone();
    TVector<uint8> Encoded;
    ASSERT_TRUE(Qoa::Encode(Source.GetData(), kFrames, kChannels, kRate, Encoded));
    EXPECT_TRUE(Qoa::IsQoa(Encoded.GetData(), Encoded.Num()));
    EXPECT_LT((uint64)Encoded.Num(), kFrames * kChannels * sizeof(int16) / 4) << "QOA stores 3.2 bits a sample against 16";

    FAudioInfo Info;
    TVector<float> Decoded;
    ASSERT_TRUE(DecodePCM(Encoded.GetData(), Encoded.Num(), Info, Decoded));
    EXPECT_EQ(Info.SampleRate, kRate);
    EXPECT_EQ(Info.NumChannels, kChannels);
    ASSERT_EQ(Info.NumFrames, kFrames);

    double ErrorSquared = 0.0;
    double SignalSquared = 0.0;
    for (int32 i = 0; i < Source.Num(); ++i)
    {
        const double Diff = Decoded[i] - Source[i];
        ErrorSquared += Diff * Diff;
        SignalSquared += (double)Source[i] * Source[i];
    }
    const double SignalToNoise = 10.0 * std::log10(SignalSquared / Math::Max(ErrorSquared, 1e-20));
    EXPECT_GT(SignalToNoise, 30.0) << "a steady tone should come back well above the quantization noise";
}

TEST(QoaCodec, RandomAccessMatchesASequentialDecode)
{
    const TVector<float> Source = MakeTone();
    TVector<uint8> Encoded;
    ASSERT_TRUE(Qoa::Encode(Source.GetData(), kFrames, kChannels, kRate, Encoded));

    FAudioInfo Info;
    TVector<float> Full;
    ASSERT_TRUE(DecodePCM(Encoded.GetData(), Encoded.Num(), Info, Full));

    FAudioReader Reader;
    ASSERT_TRUE(Reader.Open(Encoded.GetData(), Encoded.Num()));

    // Starts inside the second frame and crosses into the third, then jumps back to the first and out to the partial last.
    const uint64 Offsets[] = { Qoa::kFrameLen + 77, 3, kFrames - 10 };
    for (const uint64 Offset : Offsets)
    {
        TVector<float> Window(IntCastChecked<int32>(6000 * kChannels), 0.0f);
        const uint64 Read = Reader.ReadFrames(Offset, 6000, Window.GetData());
        ASSERT_EQ(Read, Math::Min<uint64>(6000, kFrames - Offset));
        for (int32 i = 0; i < (int32)(Read * kChannels); ++i)
        {
            ASSERT_EQ(Window[i], Full[(int32)(Offset * kChannels) + i]) << "offset " << Offset << " sample " << i;
        }
    }
}

TEST(QoaCodec, CompressAcceptsAWaveImageAndRejectsGarbage)
{
    TVector<uint8> Garbage(64, 0xAB);
    TVector<uint8> Out;
    EXPECT_FALSE(Compress(Garbage.GetData(), Garbage.Num(), Out));
    EXPECT_FALSE(IsCompressed(Garbage.GetData(), Garbage.Num()));
}
