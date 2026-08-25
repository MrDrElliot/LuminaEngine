#include <gtest/gtest.h>

#include "Assets/AssetTypes/Audio/AudioGraph.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/AssetTypes/Audio/SoundBase.h"
#include "Core/Object/ObjectCore.h"
#include "World/Entity/Components/AudioSourceComponent.h"

using namespace Lumina;

// The calls a gameplay script makes on a sound source, pinned so a refactor cannot quietly move them.
TEST(AudioGraphApi, ComponentSurfaceIsSafeWithoutADevice)
{
    SAudioSourceComponent Source;

    Source.Volume       = 0.8f;
    Source.Pitch        = 1.0f;
    Source.Bus          = EAudioBus::SFX;
    Source.bSpatialized = true;
    Source.bLooping     = true;

    // Every parameter write is a no-op until a graph instance exists, never a crash.
    Source.SetFloatParameter("Throttle", 0.5f);
    Source.SetIntParameter("Surface", 2);
    Source.SetBoolParameter("Boost", true);
    Source.TriggerParameter("Fire");

    EXPECT_EQ(Source.GetFloatOutput("Pulse"), 0.0f);
    EXPECT_FALSE(Source.IsPlaying());
    EXPECT_EQ(Source.GetPlaybackTime(), 0.0f);

    Source.Stop();
    EXPECT_FALSE(Source.bPlaying);
}

// bLooping speaks for a wave only. A graph's own construction decides whether it plays until stopped,
// which is what the audio system virtualizes on.
TEST(AudioGraphApi, PersistenceComesFromTheSoundNotTheLoopFlag)
{
    SAudioSourceComponent Source;

    // No sound at all falls back to the wave rule, so the flag still reads through.
    Source.bLooping = false;
    EXPECT_FALSE(Source.IsPersistent());
    Source.bLooping = true;
    EXPECT_TRUE(Source.IsPersistent());

    CAudioGraph* Graph = NewObject<CAudioGraph>();
    Source.Sound = Graph;

    // An uncompiled graph has no wired On Finished, so it reads as endless whatever bLooping says.
    Source.bLooping = false;
    EXPECT_TRUE(Source.IsPersistent());
    Source.bLooping = true;
    EXPECT_TRUE(Source.IsPersistent());

    FAudioGraphProgram Program;
    Program.Nodes.push_back(FAudioGraphNodeInstance());
    Program.FinishedSlot = 0;
    Graph->SetProgram(Move(Program), TVector<TObjectPtr<CAudioStream>>());

    // A wired On Finished makes it a one shot, which must never be restarted by virtualization.
    EXPECT_FALSE(Source.IsPersistent());
    Source.bLooping = false;
    EXPECT_FALSE(Source.IsPersistent());
}

// A wave and a graph must both satisfy the one Sound slot the component exposes.
TEST(AudioGraphApi, BothSoundKindsFitTheOneSlot)
{
    EXPECT_TRUE(CAudioStream::StaticClass()->IsChildOf(CSoundBase::StaticClass()));
    EXPECT_TRUE(CAudioGraph::StaticClass()->IsChildOf(CSoundBase::StaticClass()));

    SAudioSourceComponent Source;
    EXPECT_EQ(Source.Sound, nullptr);
}
