#include <gtest/gtest.h>

#include "Audio/AudioMixer.h"
#include "Audio/AudioSource.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kMixRate = 48000;

    /** Emits a caller-supplied ramp so a test can tell which source frame landed where. */
    class FMixerTestSource final : public IAudioSource
    {
    public:

        FMixerTestSource(uint32 InRate, uint32 InChannels, uint32 InFrames, float InStart = 1.0f, float InStep = 1.0f)
            : Rate(InRate)
            , Channels(InChannels)
            , TotalFrames(InFrames)
            , Start(InStart)
            , Step(InStep)
        {
        }

        uint32 Pull(float* Out, uint32 NumFrames) override
        {
            uint32 Written = 0;
            while (Written < NumFrames)
            {
                if (Cursor >= TotalFrames)
                {
                    if (!bLooping)
                    {
                        break;
                    }
                    Cursor = 0;
                }

                for (uint32 c = 0; c < Channels; ++c)
                {
                    Out[(size_t)Written * Channels + c] = Start + (float)Cursor * Step;
                }

                ++Cursor;
                ++Written;
            }
            return Written;
        }

        uint32 GetSampleRate() const override { return Rate; }
        uint32 GetChannelCount() const override { return Channels; }
        bool IsAtEnd() const override { return !bLooping && Cursor >= TotalFrames; }
        uint64 GetCursor() const override { return Cursor; }
        void Seek(uint64 Frame) override { Cursor = TotalFrames != 0 ? Frame % TotalFrames : 0; }
        void SetLooping(bool bInLooping) override { bLooping = bInLooping; }
        bool IsSeekable() const override { return true; }

    private:

        uint32 Rate = kMixRate;
        uint32 Channels = 1;
        uint32 TotalFrames = 0;
        float  Start = 1.0f;
        float  Step = 1.0f;
        uint64 Cursor = 0;
        bool   bLooping = false;
    };

    /** A source that never ends, so it stands in for a live stream or a graph. */
    class FMixerConstantSource final : public IAudioSource
    {
    public:

        FMixerConstantSource(uint32 InChannels, float InValue) : Channels(InChannels), Value(InValue) {}

        uint32 Pull(float* Out, uint32 NumFrames) override
        {
            for (size_t i = 0; i < (size_t)NumFrames * Channels; ++i)
            {
                Out[i] = Value;
            }
            return NumFrames;
        }

        uint32 GetSampleRate() const override { return kMixRate; }
        uint32 GetChannelCount() const override { return Channels; }
        bool IsAtEnd() const override { return false; }

    private:

        uint32 Channels = 1;
        float Value = 0.0f;
    };

    FMixerVoiceDesc MakeDesc(uint32 Slot, IAudioSource* Source, float Volume = 1.0f)
    {
        FMixerVoiceDesc Desc;
        Desc.Slot = Slot;
        Desc.Generation = Slot + 1;
        Desc.Source = Source;
        Desc.Params.Volume = Volume;
        return Desc;
    }

    float MixerPeak(const TVector<float>& Samples)
    {
        float Peak = 0.0f;
        for (float Sample : Samples)
        {
            Peak = Math::Max(Peak, Sample < 0.0f ? -Sample : Sample);
        }
        return Peak;
    }
}

TEST(AudioMixer, RejectsDegenerateFormat)
{
    FAudioMixer Mixer;
    EXPECT_FALSE(Mixer.Initialize(0, 2));
    EXPECT_FALSE(Mixer.Initialize(48000, 0));
    EXPECT_FALSE(Mixer.IsInitialized());
}

TEST(AudioMixer, RendersSilenceWithNoVoices)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));

    TVector<float> Out(512, 1234.0f);
    Mixer.RenderAudio(Out.data(), 256);

    EXPECT_FLOAT_EQ(MixerPeak(Out), 0.0f);
    EXPECT_EQ(Mixer.GetActiveVoiceCount(), 0u);
}

TEST(AudioMixer, PassesAMatchingRateSourceThroughUnchanged)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));
    Mixer.SetVolumeSmoothing(0.0f);

    // Values stay well under one, so the master clamp never masks a resampling error.
    FMixerTestSource Source(kMixRate, 1, 64, 0.001f, 0.01f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &Source)));

    TVector<float> Out(128, -1.0f);
    Mixer.RenderAudio(Out.data(), 64);

    // A mono source is replicated to both output channels, and frame N carries the Nth source value.
    for (uint32 Frame = 0; Frame < 64; ++Frame)
    {
        const float Expected = 0.001f + (float)Frame * 0.01f;
        EXPECT_NEAR(Out[Frame * 2 + 0], Expected, 1.0e-4f) << "frame " << Frame;
        EXPECT_FLOAT_EQ(Out[Frame * 2 + 0], Out[Frame * 2 + 1]) << "frame " << Frame;
    }
}

TEST(AudioMixer, AppliesBusAndMasterVolume)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));
    Mixer.SetVolumeSmoothing(0.0f);

    FMixerConstantSource Source(1, 0.5f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &Source)));

    TVector<float> Out(64, 0.0f);
    Mixer.RenderAudio(Out.data(), 32);
    EXPECT_NEAR(Out[0], 0.5f, 1.0e-5f);

    Mixer.SetBusVolume(EAudioBus::SFX, 0.5f);
    Mixer.RenderAudio(Out.data(), 32);
    EXPECT_NEAR(Out[0], 0.25f, 1.0e-5f);

    Mixer.SetBusVolume(EAudioBus::Master, 0.5f);
    Mixer.RenderAudio(Out.data(), 32);
    EXPECT_NEAR(Out[0], 0.125f, 1.0e-5f);

    Mixer.SetBusMuted(EAudioBus::SFX, true);
    Mixer.RenderAudio(Out.data(), 32);
    EXPECT_FLOAT_EQ(Out[0], 0.0f);
}

TEST(AudioMixer, PansAcrossTheStereoField)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));
    Mixer.SetVolumeSmoothing(0.0f);

    FMixerConstantSource Source(1, 0.5f);
    FMixerVoiceDesc Desc = MakeDesc(0, &Source);
    Desc.Params.Attenuation.Pan = -1.0f;
    ASSERT_TRUE(Mixer.StartVoice(Desc));

    TVector<float> Out(64, 0.0f);
    Mixer.RenderAudio(Out.data(), 32);

    EXPECT_NEAR(Out[0], 0.5f, 1.0e-5f);
    EXPECT_NEAR(Out[1], 0.0f, 1.0e-5f);

    Mixer.PostCommand(FAudioCommand::MakeFloat(EAudioCommandType::SetPan, FAudioHandle{ 1, 0 }, 1.0f));

    // A gain change ramps across one block, so the settled value is only there on the block after it.
    Mixer.RenderAudio(Out.data(), 32);
    Mixer.RenderAudio(Out.data(), 32);

    EXPECT_NEAR(Out[0], 0.0f, 1.0e-5f);
    EXPECT_NEAR(Out[1], 0.5f, 1.0e-5f);
}

TEST(AudioMixer, ResamplesAHalfRateSourceToTheDeviceRate)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 1));
    Mixer.SetVolumeSmoothing(0.0f);

    // At half the mixer rate each source frame spans two output frames, so the ramp advances half as fast.
    FMixerTestSource Source(kMixRate / 2, 1, 128, 0.0f, 0.1f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &Source)));

    TVector<float> Out(64, -1.0f);
    Mixer.RenderAudio(Out.data(), 64);

    EXPECT_NEAR(Out[0], 0.0f, 1.0e-4f);
    EXPECT_NEAR(Out[2], 0.1f, 1.0e-4f);
    EXPECT_NEAR(Out[4], 0.2f, 1.0e-4f);
    EXPECT_NEAR(Out[6], 0.3f, 1.0e-4f);

    // The odd frames sit halfway between two source frames.
    EXPECT_NEAR(Out[1], 0.05f, 1.0e-4f);
    EXPECT_NEAR(Out[3], 0.15f, 1.0e-4f);
}

TEST(AudioMixer, FreesTheSlotWhenASourceRunsOut)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));

    FMixerTestSource Source(kMixRate, 1, 16, 0.001f, 0.01f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &Source)));

    TVector<float> Out(512, 0.0f);

    Mixer.RenderAudio(Out.data(), 8);
    EXPECT_EQ(Mixer.GetSlotState(0), EAudioVoiceState::Playing);

    for (uint32 i = 0; i < 4; ++i)
    {
        Mixer.RenderAudio(Out.data(), 64);
    }

    EXPECT_EQ(Mixer.GetSlotState(0), EAudioVoiceState::Free);
    EXPECT_EQ(Mixer.GetActiveVoiceCount(), 0u);
}

TEST(AudioMixer, AGeneratedVoiceIsNeverAutoCollected)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));

    FMixerConstantSource Source(1, 0.25f);
    FMixerVoiceDesc Desc = MakeDesc(0, &Source);
    Desc.bGenerated = true;
    ASSERT_TRUE(Mixer.StartVoice(Desc));

    TVector<float> Out(512, 0.0f);
    for (uint32 i = 0; i < 16; ++i)
    {
        Mixer.RenderAudio(Out.data(), 256);
    }

    EXPECT_EQ(Mixer.GetSlotState(0), EAudioVoiceState::Playing);
}

TEST(AudioMixer, StopCommandsFreeVoices)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));

    FMixerConstantSource First(1, 0.25f);
    FMixerConstantSource Second(1, 0.25f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &First)));
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(1, &Second)));

    TVector<float> Out(512, 0.0f);
    Mixer.RenderAudio(Out.data(), 64);
    EXPECT_EQ(Mixer.GetActiveVoiceCount(), 2u);

    Mixer.PostCommand(FAudioCommand::MakeStop(FAudioHandle{ 1, 0 }, EAudioStopMode::Immediate, 0.0f));
    Mixer.RenderAudio(Out.data(), 64);
    EXPECT_EQ(Mixer.GetSlotState(0), EAudioVoiceState::Free);
    EXPECT_EQ(Mixer.GetSlotState(1), EAudioVoiceState::Playing);

    Mixer.PostCommand(FAudioCommand::MakeStopAll(EAudioStopMode::Immediate, 0.0f));
    Mixer.RenderAudio(Out.data(), 64);
    EXPECT_EQ(Mixer.GetSlotState(1), EAudioVoiceState::Free);
    EXPECT_EQ(Mixer.GetActiveVoiceCount(), 0u);
}

TEST(AudioMixer, PausedVoicesHoldTheirSlotAndGoQuiet)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));
    Mixer.SetVolumeSmoothing(0.0f);

    FMixerConstantSource Source(1, 0.5f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &Source)));

    TVector<float> Out(64, 0.0f);
    Mixer.RenderAudio(Out.data(), 32);
    EXPECT_NEAR(Out[0], 0.5f, 1.0e-5f);

    Mixer.PostCommand(FAudioCommand::MakeBool(EAudioCommandType::SetPaused, FAudioHandle{ 1, 0 }, true));
    Mixer.RenderAudio(Out.data(), 32);

    EXPECT_FLOAT_EQ(MixerPeak(Out), 0.0f);
    EXPECT_EQ(Mixer.GetSlotState(0), EAudioVoiceState::Paused);
    EXPECT_EQ(Mixer.GetActiveVoiceCount(), 1u);
}

TEST(AudioMixer, ReverbSendAddsAWetTail)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));
    Mixer.SetVolumeSmoothing(0.0f);

    FAudioReverbParams Params;
    Params.WetLevel = 1.0f;
    Mixer.SetReverbParams(Params);
    Mixer.SetBusReverbSend(EAudioBus::SFX, 1.0f);

    FMixerConstantSource Source(1, 0.5f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &Source)));

    TVector<float> Out(512, 0.0f);

    // Runs past the reverb pre-delay, then stops the voice so only the tail is left.
    for (uint32 i = 0; i < 16; ++i)
    {
        Mixer.RenderAudio(Out.data(), 256);
    }

    Mixer.PostCommand(FAudioCommand::MakeStopAll(EAudioStopMode::Immediate, 0.0f));
    Mixer.RenderAudio(Out.data(), 256);
    ASSERT_EQ(Mixer.GetActiveVoiceCount(), 0u);

    Mixer.RenderAudio(Out.data(), 256);
    EXPECT_GT(MixerPeak(Out), 0.0f);
}

TEST(AudioMixer, SpatializedVoicesFallOffWithDistance)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));
    Mixer.SetVolumeSmoothing(0.0f);
    Mixer.SetListener(0, FVector3(0.0f), FQuat(1.0f, 0.0f, 0.0f, 0.0f), FVector3(0.0f));

    FMixerConstantSource Source(1, 1.0f);
    FMixerVoiceDesc Desc = MakeDesc(0, &Source);
    Desc.Params.bSpatialized = true;
    Desc.Params.Position = FVector3(1.0f, 0.0f, 0.0f);
    Desc.Params.Attenuation.MinDistance = 1.0f;
    Desc.Params.Attenuation.MaxDistance = 100.0f;
    ASSERT_TRUE(Mixer.StartVoice(Desc));

    TVector<float> Out(64, 0.0f);
    Mixer.RenderAudio(Out.data(), 32);
    const float NearPeak = MixerPeak(Out);
    EXPECT_GT(NearPeak, 0.0f);

    Mixer.PostCommand(FAudioCommand::MakeVector(EAudioCommandType::SetPosition,
        FAudioHandle{ 1, 0 }, FVector3(50.0f, 0.0f, 0.0f)));

    // A gain change ramps across one block, so the settled level is only there on the block after it.
    Mixer.RenderAudio(Out.data(), 32);
    Mixer.RenderAudio(Out.data(), 32);

    EXPECT_LT(MixerPeak(Out), NearPeak);
}

TEST(AudioMixer, RespectsTheVoiceBudget)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));
    Mixer.SetMaxVoiceCount(2);

    FMixerConstantSource A(1, 0.1f);
    FMixerConstantSource B(1, 0.1f);
    FMixerConstantSource C(1, 0.1f);
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(0, &A)));
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(1, &B)));
    ASSERT_TRUE(Mixer.StartVoice(MakeDesc(2, &C)));

    TVector<float> Out(512, 0.0f);
    Mixer.RenderAudio(Out.data(), 64);

    EXPECT_EQ(Mixer.GetActiveVoiceCount(), 2u);
}

TEST(AudioMixer, RenderCountAdvancesPerCallback)
{
    FAudioMixer Mixer;
    ASSERT_TRUE(Mixer.Initialize(kMixRate, 2));

    const uint64 Before = Mixer.GetRenderCount();

    TVector<float> Out(512, 0.0f);
    Mixer.RenderAudio(Out.data(), 256);
    Mixer.RenderAudio(Out.data(), 256);

    EXPECT_EQ(Mixer.GetRenderCount(), Before + 2);
}
