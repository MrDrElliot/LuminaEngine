#include <gtest/gtest.h>

#include "Audio/AudioReverb.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"

using namespace Lumina;

namespace
{
    float ReverbPeakOf(const TVector<float>& Samples)
    {
        float Peak = 0.0f;
        for (float Sample : Samples)
        {
            const float Magnitude = Sample < 0.0f ? -Sample : Sample;
            if (Magnitude > Peak)
            {
                Peak = Magnitude;
            }
        }
        return Peak;
    }

    // The longest comb delay is over 1700 frames at 48 kHz, so the wet tail needs several blocks to appear.
    float RenderPeakOverBlocks(FAudioReverb& Reverb, const float* FirstInput, uint32 BlockFrames, uint32 Channels, uint32 Blocks)
    {
        TVector<float> Out((size_t)BlockFrames * Channels, 0.0f);

        float Peak = 0.0f;
        for (uint32 Block = 0; Block < Blocks; ++Block)
        {
            Reverb.Process(Block == 0 ? FirstInput : nullptr, Out.data(), BlockFrames);
            Peak = Math::Max(Peak, ReverbPeakOf(Out));
        }
        return Peak;
    }
}

TEST(AudioReverb, RejectsDegenerateConfiguration)
{
    FAudioReverb Reverb;

    EXPECT_FALSE(Reverb.Initialize(0, 48000));
    EXPECT_FALSE(Reverb.IsInitialized());

    EXPECT_FALSE(Reverb.Initialize(2, 0));
    EXPECT_FALSE(Reverb.IsInitialized());
}

TEST(AudioReverb, ClampsChannelCountToTheMaximum)
{
    FAudioReverb Reverb;
    ASSERT_TRUE(Reverb.Initialize(64, 48000));
    EXPECT_EQ(Reverb.GetChannelCount(), FAudioReverb::MaxChannels);
    Reverb.Shutdown();
}

TEST(AudioReverb, ProcessBeforeInitializeIsANoOp)
{
    FAudioReverb Reverb;

    TVector<float> Out(64, 1234.0f);
    Reverb.Process(nullptr, Out.data(), 32);

    // Untouched rather than zeroed, so a caller cannot mistake a dead reverb for silence.
    EXPECT_FLOAT_EQ(Out[0], 1234.0f);
}

TEST(AudioReverb, RoundTripsParameters)
{
    FAudioReverb Reverb;
    ASSERT_TRUE(Reverb.Initialize(2, 48000));

    FAudioReverbParams Params;
    Params.RoomSize = 0.8f;
    Params.Damping  = 0.2f;
    Params.Width    = 0.5f;
    Params.WetLevel = 0.6f;
    Reverb.SetParams(Params);

    const FAudioReverbParams Read = Reverb.GetParams();
    EXPECT_FLOAT_EQ(Read.RoomSize, 0.8f);
    EXPECT_FLOAT_EQ(Read.Damping, 0.2f);
    EXPECT_FLOAT_EQ(Read.Width, 0.5f);
    EXPECT_FLOAT_EQ(Read.WetLevel, 0.6f);

    // Out of range values are clamped on the way in.
    FAudioReverbParams Extreme;
    Extreme.RoomSize = 5.0f;
    Extreme.Damping  = -1.0f;
    Extreme.WetLevel = -2.0f;
    Reverb.SetParams(Extreme);

    const FAudioReverbParams Clamped = Reverb.GetParams();
    EXPECT_FLOAT_EQ(Clamped.RoomSize, 1.0f);
    EXPECT_FLOAT_EQ(Clamped.Damping, 0.0f);
    EXPECT_FLOAT_EQ(Clamped.WetLevel, 0.0f);

    Reverb.Shutdown();
}

TEST(AudioReverb, ImpulseProducesADecayingTail)
{
    constexpr uint32 Channels = 2;
    constexpr uint32 BlockFrames = 512;

    FAudioReverb Reverb;
    ASSERT_TRUE(Reverb.Initialize(Channels, 48000));
    EXPECT_TRUE(Reverb.IsInitialized());

    TVector<float> Impulse((size_t)BlockFrames * Channels, 0.0f);
    Impulse[0] = 1.0f;
    Impulse[1] = 1.0f;

    const float EarlyPeak = RenderPeakOverBlocks(Reverb, Impulse.data(), BlockFrames, Channels, 8);
    EXPECT_GT(EarlyPeak, 0.0f);

    const float LatePeak = RenderPeakOverBlocks(Reverb, nullptr, BlockFrames, Channels, 400);
    EXPECT_LT(LatePeak, EarlyPeak);

    Reverb.Shutdown();
    EXPECT_FALSE(Reverb.IsInitialized());
}

TEST(AudioReverb, ReinitializeResetsTheTail)
{
    constexpr uint32 Channels = 2;
    constexpr uint32 BlockFrames = 512;

    FAudioReverb Reverb;
    ASSERT_TRUE(Reverb.Initialize(Channels, 48000));

    TVector<float> Impulse((size_t)BlockFrames * Channels, 0.0f);
    Impulse[0] = 1.0f;
    Impulse[1] = 1.0f;

    ASSERT_GT(RenderPeakOverBlocks(Reverb, Impulse.data(), BlockFrames, Channels, 8), 0.0f);

    Reverb.Shutdown();
    ASSERT_TRUE(Reverb.Initialize(Channels, 48000));

    // A fresh network has empty delay lines, so silence in is exactly silence out.
    EXPECT_FLOAT_EQ(RenderPeakOverBlocks(Reverb, nullptr, BlockFrames, Channels, 8), 0.0f);

    Reverb.Shutdown();
}
