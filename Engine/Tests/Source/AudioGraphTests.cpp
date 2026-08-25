#include <gtest/gtest.h>

#include "Audio/Graph/AudioGraphInstance.h"
#include "Audio/Graph/AudioGraphOperator.h"
#include "Audio/Graph/AudioGraphProgram.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kTestSampleRate = 48000;

    struct FProgramBuilder
    {
        FAudioGraphProgram Program;

        uint16 Slot(EAudioGraphType Type) { return Program.SlotCounts.Allocate(Type); }

        uint16 Literal(EAudioGraphType Type, float FloatValue, int32 IntValue = 0, bool BoolValue = false)
        {
            FAudioGraphSlotInit Init;
            Init.Type       = Type;
            Init.Slot       = Slot(Type);
            Init.FloatValue = FloatValue;
            Init.IntValue   = IntValue;
            Init.BoolValue  = BoolValue;
            Program.SlotInits.push_back(Init);
            return Init.Slot;
        }

        FAudioGraphNodeInstance& AddNode(const char* OperatorName)
        {
            FAudioGraphNodeInstance Node;
            Node.OperatorName = OperatorName;
            Program.Nodes.push_back(Move(Node));
            return Program.Nodes.back();
        }

        /** Fills a node's pins from the operator's signature, one fresh slot each. */
        FAudioGraphNodeInstance& AddNodeWithDefaults(const char* OperatorName)
        {
            const FAudioGraphNodeClass* Class = FAudioGraphNodeRegistry::Get().Find(FName(OperatorName));
            EXPECT_NE(Class, nullptr) << OperatorName;

            FAudioGraphNodeInstance& Node = AddNode(OperatorName);
            if (Class == nullptr)
            {
                return Node;
            }

            for (EAudioGraphType Type : Class->Signature.Inputs)
            {
                Node.InputSlots.push_back(Literal(Type, 0.0f));
            }

            for (EAudioGraphType Type : Class->Signature.Outputs)
            {
                Node.OutputSlots.push_back(Slot(Type));
            }

            return Node;
        }

        /** Sine wired to a real frequency, which a signature driven default cannot supply. */
        FAudioGraphNodeInstance& AddSine(float Frequency)
        {
            FAudioGraphNodeInstance& Node = AddNode("Sine");
            Node.InputSlots.push_back(Literal(EAudioGraphType::Float, Frequency));
            Node.InputSlots.push_back(Slot(EAudioGraphType::Audio));
            Node.InputSlots.push_back(Slot(EAudioGraphType::Trigger));
            Node.OutputSlots.push_back(Slot(EAudioGraphType::Audio));
            return Node;
        }
    };

    float PeakOf(const TVector<float>& Samples)
    {
        float Peak = 0.0f;
        for (float Sample : Samples)
        {
            Peak = Math::Max(Peak, Math::Abs(Sample));
        }
        return Peak;
    }
}

// The builtin node library self registers at static init, so a program can name any of it.
TEST(AudioGraph, BuiltinNodesAreRegistered)
{
    const FAudioGraphNodeRegistry& Registry = FAudioGraphNodeRegistry::Get();

    EXPECT_GT(Registry.GetAll().size(), 20u);
    EXPECT_NE(Registry.Find(FName("Sine")), nullptr);
    EXPECT_NE(Registry.Find(FName("Gain")), nullptr);
    EXPECT_NE(Registry.Find(FName("ADSR")), nullptr);
    EXPECT_NE(Registry.Find(FName("BiquadFilter")), nullptr);
    EXPECT_EQ(Registry.Find(FName("NoSuchNode")), nullptr);
}

// Every registered node must build and run a block without touching memory it does not own.
TEST(AudioGraph, EveryNodeRendersABlock)
{
    for (const FAudioGraphNodeClass& Class : FAudioGraphNodeRegistry::Get().GetAll())
    {
        FProgramBuilder Builder;
        Builder.AddNodeWithDefaults(Class.Name.c_str());

        FAudioGraphInstance Instance;
        ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2)) << Class.Name.c_str();

        TVector<float> Output((size_t)kAudioGraphBlockFrames * 2, 0.0f);
        Instance.Render(Output.data(), kAudioGraphBlockFrames);
    }
}

// A sine wired straight to the output must produce a signal that swings the full unit range.
TEST(AudioGraph, SineReachesTheOutput)
{
    FProgramBuilder Builder;

    FAudioGraphNodeInstance& Sine = Builder.AddSine(440.0f);
    const uint16 SineOut = Sine.OutputSlots[0];

    Builder.Program.OutputLeftSlot  = SineOut;
    Builder.Program.OutputRightSlot = SineOut;

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));

    TVector<float> Output((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    Instance.Render(Output.data(), kAudioGraphBlockFrames);

    EXPECT_GT(PeakOf(Output), 0.9f);

    for (size_t Frame = 0; Frame < kAudioGraphBlockFrames; ++Frame)
    {
        ASSERT_FLOAT_EQ(Output[Frame * 2], Output[Frame * 2 + 1]);
    }
}

// Render must be independent of how the mixer splits its requests across block boundaries.
TEST(AudioGraph, RenderIsIndependentOfRequestSize)
{
    auto RenderWith = [](uint32 ChunkFrames)
    {
        FProgramBuilder Builder;
        FAudioGraphNodeInstance& Sine = Builder.AddSine(440.0f);
        Builder.Program.OutputLeftSlot  = Sine.OutputSlots[0];
        Builder.Program.OutputRightSlot = Sine.OutputSlots[0];

        FAudioGraphInstance Instance;
        EXPECT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));

        constexpr uint32 TotalFrames = kAudioGraphBlockFrames * 3;
        TVector<float> Output((size_t)TotalFrames * 2, 0.0f);

        for (uint32 Written = 0; Written < TotalFrames; Written += ChunkFrames)
        {
            const uint32 Frames = Math::Min(ChunkFrames, TotalFrames - Written);
            Instance.Render(Output.data() + (size_t)Written * 2, Frames);
        }

        return Output;
    };

    const TVector<float> WholeBlocks = RenderWith(kAudioGraphBlockFrames);
    const TVector<float> Fragments   = RenderWith(37);

    ASSERT_EQ(WholeBlocks.size(), Fragments.size());
    for (size_t Index = 0; Index < WholeBlocks.size(); ++Index)
    {
        ASSERT_NEAR(WholeBlocks[Index], Fragments[Index], 1.0e-5f) << "at sample " << Index;
    }
}

// Gain reads its multiplier from a graph input, which is the whole point of a parameterized sound.
TEST(AudioGraph, FloatParameterDrivesGain)
{
    FProgramBuilder Builder;

    FAudioGraphNodeInstance& Sine = Builder.AddSine(440.0f);

    const FAudioGraphNodeClass* GainClass = FAudioGraphNodeRegistry::Get().Find(FName("Gain"));
    ASSERT_NE(GainClass, nullptr);

    const uint16 GainSlot = Builder.Literal(EAudioGraphType::Float, 1.0f);

    FAudioGraphNodeInstance& Gain = Builder.AddNode("Gain");
    Gain.InputSlots.push_back(Sine.OutputSlots[0]);
    Gain.InputSlots.push_back(GainSlot);
    Gain.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Audio));

    Builder.Program.OutputLeftSlot  = Gain.OutputSlots[0];
    Builder.Program.OutputRightSlot = Gain.OutputSlots[0];

    FAudioGraphParameterDecl Decl;
    Decl.Name         = "Volume";
    Decl.Type         = EAudioGraphType::Float;
    Decl.Slot         = GainSlot;
    Decl.DefaultFloat = 1.0f;
    Builder.Program.Inputs.push_back(Decl);

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));

    TVector<float> Loud((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    Instance.Render(Loud.data(), kAudioGraphBlockFrames);
    EXPECT_GT(PeakOf(Loud), 0.9f);

    EXPECT_TRUE(Instance.SetFloatParameter(FName("Volume"), 0.0f));
    EXPECT_FALSE(Instance.SetFloatParameter(FName("NoSuchParameter"), 0.0f));

    // The first block after the write still ramps down from the old gain, so measure the one after it.
    TVector<float> Ramp((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    Instance.Render(Ramp.data(), kAudioGraphBlockFrames);

    TVector<float> Silent((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    Instance.Render(Silent.data(), kAudioGraphBlockFrames);
    EXPECT_LT(PeakOf(Silent), 1.0e-6f);
}

// The OnPlay trigger fires itself on the first block, so a one shot needs no gameplay call to start.
TEST(AudioGraph, OnPlayFiresOnTheFirstBlock)
{
    FProgramBuilder Builder;

    const uint16 OnPlaySlot = Builder.Slot(EAudioGraphType::Trigger);

    FAudioGraphParameterDecl OnPlay;
    OnPlay.Name = FName(kAudioGraphOnPlayInput);
    OnPlay.Type = EAudioGraphType::Trigger;
    OnPlay.Slot = OnPlaySlot;
    Builder.Program.Inputs.push_back(OnPlay);

    FAudioGraphNodeInstance& Envelope = Builder.AddNode("AttackDecay");
    Envelope.InputSlots.push_back(OnPlaySlot);
    Envelope.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 0.001f));
    Envelope.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 1.0f));
    Envelope.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 1.0f));
    Envelope.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Audio));
    Envelope.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Trigger));

    Builder.Program.OutputLeftSlot  = Envelope.OutputSlots[0];
    Builder.Program.OutputRightSlot = Envelope.OutputSlots[0];

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));

    TVector<float> Output((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    Instance.Render(Output.data(), kAudioGraphBlockFrames);

    EXPECT_GT(PeakOf(Output), 0.0f);
}

// A voice retires when the graph raises its finished output, which is what auto collects a one shot.
TEST(AudioGraph, FinishedTriggerRetiresTheVoice)
{
    FProgramBuilder Builder;

    const uint16 OnPlaySlot = Builder.Slot(EAudioGraphType::Trigger);

    FAudioGraphParameterDecl OnPlay;
    OnPlay.Name = FName(kAudioGraphOnPlayInput);
    OnPlay.Type = EAudioGraphType::Trigger;
    OnPlay.Slot = OnPlaySlot;
    Builder.Program.Inputs.push_back(OnPlay);

    FAudioGraphNodeInstance& Envelope = Builder.AddNode("AttackDecay");
    Envelope.InputSlots.push_back(OnPlaySlot);
    Envelope.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 0.0001f));
    Envelope.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 0.0001f));
    Envelope.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 1.0f));
    Envelope.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Audio));
    Envelope.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Trigger));

    Builder.Program.OutputLeftSlot  = Envelope.OutputSlots[0];
    Builder.Program.OutputRightSlot = Envelope.OutputSlots[0];
    Builder.Program.FinishedSlot    = Envelope.OutputSlots[1];

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));
    EXPECT_FALSE(Instance.IsFinished());

    TVector<float> Output((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    for (uint32 Block = 0; Block < 8 && !Instance.IsFinished(); ++Block)
    {
        Instance.Render(Output.data(), kAudioGraphBlockFrames);
    }

    EXPECT_TRUE(Instance.IsFinished());
}

// A graph trigger reaches gameplay as a monotonic count, which is what lets a sound drive an event.
TEST(AudioGraph, TriggerOutputsAreCounted)
{
    FProgramBuilder Builder;

    const uint16 OnPlaySlot = Builder.Slot(EAudioGraphType::Trigger);

    FAudioGraphParameterDecl OnPlay;
    OnPlay.Name = FName(kAudioGraphOnPlayInput);
    OnPlay.Type = EAudioGraphType::Trigger;
    OnPlay.Slot = OnPlaySlot;
    Builder.Program.Inputs.push_back(OnPlay);

    // Repeats every 64 frames, so one 256 frame block carries several fires.
    FAudioGraphNodeInstance& Repeat = Builder.AddNode("TriggerRepeat");
    Repeat.InputSlots.push_back(OnPlaySlot);
    Repeat.InputSlots.push_back(Builder.Slot(EAudioGraphType::Trigger));
    Repeat.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 64.0f / (float)kTestSampleRate));
    Repeat.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Trigger));

    FAudioGraphParameterDecl Beat;
    Beat.Name = "OnBeat";
    Beat.Type = EAudioGraphType::Trigger;
    Beat.Slot = Repeat.OutputSlots[0];
    Builder.Program.Outputs.push_back(Beat);

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("OnBeat")), 0u);

    TVector<float> Output((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    Instance.Render(Output.data(), kAudioGraphBlockFrames);

    const uint32 AfterFirstBlock = Instance.GetTriggerOutputCount(FName("OnBeat"));
    EXPECT_GE(AfterFirstBlock, 4u);

    Instance.Render(Output.data(), kAudioGraphBlockFrames);
    EXPECT_GT(Instance.GetTriggerOutputCount(FName("OnBeat")), AfterFirstBlock);

    // A name that is not a trigger output reads zero rather than another output's count.
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("NoSuchOutput")), 0u);
}

// A program from another layout must be refused outright rather than read field by field.
TEST(AudioGraph, StaleProgramIsRefused)
{
    FAudioGraphProgram Program;
    EXPECT_FALSE(Program.IsValid());

    FProgramBuilder Builder;
    Builder.AddSine(440.0f);
    EXPECT_TRUE(Builder.Program.IsValid());

    Builder.Program.Version = kAudioGraphProgramVersion + 1;
    EXPECT_FALSE(Builder.Program.IsValid());

    FAudioGraphInstance Instance;
    EXPECT_FALSE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));
}

// An unbound pin has to read silence, so a half wired graph cannot pick up another node's samples.
TEST(AudioGraph, UnboundPinsReadSilence)
{
    FProgramBuilder Builder;

    FAudioGraphNodeInstance& Sine = Builder.AddSine(440.0f);

    FAudioGraphNodeInstance& Gain = Builder.AddNode("Gain");
    Gain.InputSlots.push_back(kAudioGraphInvalidSlot);
    Gain.InputSlots.push_back(Builder.Literal(EAudioGraphType::Float, 1.0f));
    Gain.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Audio));

    Builder.Program.OutputLeftSlot  = Gain.OutputSlots[0];
    Builder.Program.OutputRightSlot = Gain.OutputSlots[0];

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));

    TVector<float> Output((size_t)kAudioGraphBlockFrames * 2, 0.0f);
    Instance.Render(Output.data(), kAudioGraphBlockFrames);

    EXPECT_EQ(PeakOf(Output), 0.0f);
    EXPECT_NE(Sine.OutputSlots[0], kAudioGraphInvalidSlot);
}
