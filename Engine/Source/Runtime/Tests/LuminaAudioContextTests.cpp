#include <gtest/gtest.h>

#include "Audio/AudioTypes.h"
#include "Audio/LuminaAudioContext.h"
#include "Audio/ProceduralAudioStream.h"
#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "TaskSystem/Fork/ForkRuntime.h"

using namespace Lumina;

namespace
{
    void PushU16(TVector<uint8>& Bytes, uint16 Value)
    {
        Bytes.Add((uint8)(Value & 0xFF));
        Bytes.Add((uint8)(Value >> 8));
    }

    void PushU32(TVector<uint8>& Bytes, uint32 Value)
    {
        Bytes.Add((uint8)(Value & 0xFF));
        Bytes.Add((uint8)((Value >> 8) & 0xFF));
        Bytes.Add((uint8)((Value >> 16) & 0xFF));
        Bytes.Add((uint8)((Value >> 24) & 0xFF));
    }

    void PushId(TVector<uint8>& Bytes, const char* Id)
    {
        for (uint32 i = 0; i < 4; ++i)
        {
            Bytes.Add((uint8)Id[i]);
        }
    }

    TSharedPtr<FAudioData> MakeSineClip(uint32 Frames)
    {
        TVector<int16> Samples(Frames);
        for (uint32 i = 0; i < Frames; ++i)
        {
            Samples[i] = (int16)(8000.0f * Math::Sin((float)i * 0.05f));
        }

        const uint32 DataBytes = Frames * sizeof(int16);

        TSharedPtr<FAudioData> Data = MakeShared<FAudioData>();
        TVector<uint8>& Bytes = Data->Bytes;

        PushId(Bytes, "RIFF");
        PushU32(Bytes, 36 + DataBytes);
        PushId(Bytes, "WAVE");
        PushId(Bytes, "fmt ");
        PushU32(Bytes, 16);
        PushU16(Bytes, 1);
        PushU16(Bytes, 1);
        PushU32(Bytes, 48000);
        PushU32(Bytes, 48000 * 2);
        PushU16(Bytes, 2);
        PushU16(Bytes, 16);
        PushId(Bytes, "data");
        PushU32(Bytes, DataBytes);

        const uint8* At = (const uint8*)Samples.GetData();
        Bytes.Append(At, DataBytes);
        return Data;
    }

    template <typename TPredicate>
    bool PumpUntil(FLuminaAudioContext& Context, TPredicate&& Predicate, uint32 MaxIterations = 200)
    {
        for (uint32 i = 0; i < MaxIterations; ++i)
        {
            Context.Update();
            if (Predicate())
            {
                return true;
            }
            Threading::Sleep(5);
        }

        Context.Update();
        return Predicate();
    }
}

TEST(LuminaAudioContext, StartsSilentAndReportsItsFormat)
{
    if (!Jobs::IsRuntimeRunning())
    {
        GTEST_SKIP() << "the job system is required for the audio pump";
    }

    FLuminaAudioContext Context;

    const FAudioDeviceInfo Info = Context.GetDeviceInfo();
    EXPECT_GT(Info.SampleRate, 0u);
    EXPECT_GT(Info.Channels, 0u);
    EXPECT_EQ(Info.ListenerCount, FAudioMixer::MaxListeners);

    EXPECT_EQ(Context.GetActiveVoiceCount(), 0u);
    EXPECT_FALSE(Context.IsSuspended());
}

TEST(LuminaAudioContext, PlaysAClipThroughToCompletion)
{
    if (!Jobs::IsRuntimeRunning())
    {
        GTEST_SKIP() << "the job system is required for the audio pump";
    }

    FLuminaAudioContext Context;
    Context.SetBusVolume(EAudioBus::Master, 0.0f);

    // A tenth of a second, short enough that the device drains it well inside the poll budget.
    const TSharedPtr<FAudioData> Clip = MakeSineClip(4800);

    SAudioPlayParams Params;
    const SAudioHandle Handle = Context.PlayAudio(Clip, Params);
    ASSERT_TRUE(Handle.IsValid());

    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetActiveVoiceCount() == 1u; }))
        << "the voice never reached the mixer";

    if (Context.GetDeviceInfo().PeriodFrames == 0)
    {
        GTEST_SKIP() << "no audio endpoint available, so the clip cannot drain";
    }

    // Proves the clip actually rendered rather than being collected before the mixer picked it up.
    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetPlaybackFrame(Handle) > 0u; }))
        << "the voice never advanced its cursor";

    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetVoiceState(Handle) == EAudioVoiceState::Free; }))
        << "the voice never finished";

    EXPECT_EQ(Context.GetActiveVoiceCount(), 0u);
    EXPECT_EQ(Context.GetDroppedVoiceCount(), 0u);
}

TEST(LuminaAudioContext, StopReleasesAVoiceAndItsSlot)
{
    if (!Jobs::IsRuntimeRunning())
    {
        GTEST_SKIP() << "the job system is required for the audio pump";
    }

    FLuminaAudioContext Context;
    Context.SetBusVolume(EAudioBus::Master, 0.0f);

    const TSharedPtr<FAudioData> Clip = MakeSineClip(480000);

    SAudioPlayParams Params;
    Params.bLooping = true;
    const SAudioHandle Handle = Context.PlayAudio(Clip, Params);
    ASSERT_TRUE(Handle.IsValid());

    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetActiveVoiceCount() == 1u; }));

    if (Context.GetDeviceInfo().PeriodFrames == 0)
    {
        GTEST_SKIP() << "no audio endpoint available, so the mixer never runs";
    }

    Context.StopSound(Handle, EAudioStopMode::Immediate, 0.0f);

    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetVoiceState(Handle) == EAudioVoiceState::Free; }))
        << "the stopped voice never released its slot";

    EXPECT_EQ(Context.GetActiveVoiceCount(), 0u);
}

TEST(LuminaAudioContext, ChurnsVoicesWithoutLosingSlots)
{
    if (!Jobs::IsRuntimeRunning())
    {
        GTEST_SKIP() << "the job system is required for the audio pump";
    }

    FLuminaAudioContext Context;
    Context.SetBusVolume(EAudioBus::Master, 0.0f);

    if (Context.GetDeviceInfo().PeriodFrames == 0)
    {
        GTEST_SKIP() << "no audio endpoint available, so the mixer never runs";
    }

    const TSharedPtr<FAudioData> Clip = MakeSineClip(2400);

    // Start and stop far more voices than there are slots, so a leaked slot would starve the run.
    for (uint32 Round = 0; Round < 40; ++Round)
    {
        TVector<SAudioHandle> Handles;
        for (uint32 i = 0; i < 8; ++i)
        {
            SAudioPlayParams Params;
            const SAudioHandle Handle = Context.PlayAudio(Clip, Params);
            EXPECT_TRUE(Handle.IsValid()) << "round " << Round << " voice " << i;
            Handles.Add(Handle);
        }

        Context.Update();
        Threading::Sleep(2);

        for (SAudioHandle Handle : Handles)
        {
            Context.StopSound(Handle, EAudioStopMode::Immediate, 0.0f);
        }

        Context.Update();
        Threading::Sleep(2);
    }

    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetActiveVoiceCount() == 0u; }))
        << "voices leaked their slots across the churn";
}

TEST(LuminaAudioContext, StealingPlayingVoicesKeepsSlotAccountingIntact)
{
    if (!Jobs::IsRuntimeRunning())
    {
        GTEST_SKIP() << "the job system is required for the audio pump";
    }

    FLuminaAudioContext Context;
    Context.SetBusVolume(EAudioBus::Master, 0.0f);

    if (Context.GetDeviceInfo().PeriodFrames == 0)
    {
        GTEST_SKIP() << "no audio endpoint available, so the mixer never runs";
    }

    constexpr uint32 Cap = 8;
    Context.SetMaxVoiceCount(Cap);
    const TSharedPtr<FAudioData> Clip = MakeSineClip(48000);

    // Low-priority loops fill every slot, so each later start has to steal one the mixer is still rendering.
    SAudioPlayParams Background;
    Background.bLooping = true;
    Background.Priority = 10;
    for (uint32 i = 0; i < Cap; ++i)
    {
        EXPECT_TRUE(Context.PlayAudio(Clip, Background).IsValid());
    }
    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetActiveVoiceCount() == Cap; }));

    SAudioPlayParams Urgent;
    Urgent.bLooping = true;
    Urgent.Priority = 200;
    for (uint32 Round = 0; Round < 64; ++Round)
    {
        const SAudioHandle Stolen = Context.PlayAudio(Clip, Urgent);
        EXPECT_TRUE(Stolen.IsValid()) << "round " << Round;
        Context.Update();
        Threading::Sleep(1);
        Context.StopSound(Stolen, EAudioStopMode::Immediate, 0.0f);
        ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetActiveVoiceCount() < Cap; })) << "round " << Round;
        EXPECT_TRUE(Context.PlayAudio(Clip, Background).IsValid()) << "round " << Round;
    }

    Context.StopAllSounds(EAudioStopMode::Immediate, 0.0f);
    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetActiveVoiceCount() == 0u; }))
        << "stolen slots leaked";
}

TEST(LuminaAudioContext, ProceduralStreamsPlayAndAreNeverAutoCollected)
{
    if (!Jobs::IsRuntimeRunning())
    {
        GTEST_SKIP() << "the job system is required for the audio pump";
    }

    FLuminaAudioContext Context;
    Context.SetBusVolume(EAudioBus::Master, 0.0f);

    TSharedPtr<FProceduralAudioStream> Stream = Context.CreateProceduralStream(48000, 1, 4096);
    ASSERT_TRUE(Stream);
    ASSERT_TRUE(Stream->IsValid());

    TVector<float> Block(1024, 0.25f);
    EXPECT_GT(Stream->Write(Block.GetData(), 1024), 0u);

    SAudioPlayParams Params;
    const SAudioHandle Handle = Context.PlayProceduralStream(Stream, Params);
    ASSERT_TRUE(Handle.IsValid());

    ASSERT_TRUE(PumpUntil(Context, [&]() { return Context.GetActiveVoiceCount() == 1u; }));

    if (Context.GetDeviceInfo().PeriodFrames == 0)
    {
        GTEST_SKIP() << "no audio endpoint available, so the mixer never runs";
    }

    // Well past the point the ring runs dry; a live stream underruns to silence rather than ending.
    for (uint32 i = 0; i < 20; ++i)
    {
        Context.Update();
        Threading::Sleep(5);
    }

    EXPECT_EQ(Context.GetVoiceState(Handle), EAudioVoiceState::Playing);
}
