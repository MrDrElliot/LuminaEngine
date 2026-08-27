#include <gtest/gtest.h>

#include "Audio/AudioDevice.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kDeviceTestRate = 48000;
    constexpr uint32 kDeviceTestChannels = 2;

    /** Writes silence and counts callbacks, so the test never puts noise through the user's speakers. */
    class FCountingRenderCallback final : public IAudioRenderCallback
    {
    public:

        void RenderAudio(float* Out, uint32 FrameCount) override
        {
            Memory::Memzero(Out, (size_t)FrameCount * kDeviceTestChannels * sizeof(float));
            Frames.fetch_add(FrameCount, Atomic::MemoryOrderRelease);
            Calls.fetch_add(1, Atomic::MemoryOrderRelease);
        }

        TAtomic<uint32> Calls{0};
        TAtomic<uint32> Frames{0};
    };
}

TEST(AudioDevice, OpensAnEndpointAndDrivesTheCallback)
{
    FCountingRenderCallback Callback;

    FAudioDeviceConfig Config;
    Config.SampleRate = kDeviceTestRate;
    Config.Channels   = kDeviceTestChannels;

    TUniquePtr<IAudioDevice> Device = Audio::CreateDevice(Config, &Callback);
    if (!Device)
    {
        GTEST_SKIP() << "no audio endpoint available in this session";
    }

    EXPECT_TRUE(Device->IsRunning());
    EXPECT_EQ(Device->GetSampleRate(), kDeviceTestRate);
    EXPECT_EQ(Device->GetChannelCount(), kDeviceTestChannels);
    EXPECT_GT(Device->GetPeriodFrames(), 0u);

    // A shared-mode endpoint runs on a period of a few milliseconds, so this is many callbacks' worth.
    for (uint32 Attempt = 0; Attempt < 200 && Callback.Calls.load(Atomic::MemoryOrderAcquire) < 4; ++Attempt)
    {
        Threading::Sleep(5);
    }

    EXPECT_GE(Callback.Calls.load(Atomic::MemoryOrderAcquire), 4u);
    EXPECT_GT(Callback.Frames.load(Atomic::MemoryOrderAcquire), 0u);
    EXPECT_FALSE(Device->NeedsRestart());

    Device->Stop();
    EXPECT_FALSE(Device->IsRunning());

    // Stopping joins the render thread, so no further callback can land after this point.
    const uint32 Settled = Callback.Calls.load(Atomic::MemoryOrderAcquire);
    Threading::Sleep(50);
    EXPECT_EQ(Callback.Calls.load(Atomic::MemoryOrderAcquire), Settled);
}

TEST(AudioDevice, RefusesANullCallback)
{
    FAudioDeviceConfig Config;
    EXPECT_FALSE(Audio::CreateDevice(Config, nullptr));
}
