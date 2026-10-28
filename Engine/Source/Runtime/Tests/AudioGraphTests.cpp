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

namespace
{
    // Mono wave whose sample N reads N hundredths, so an output value names the frame it came from.
    TSharedPtr<FAudioGraphWaveResource> MakeRampWave(uint32 Frames, uint32 SampleRate = kTestSampleRate)
    {
        TSharedPtr<FAudioGraphWaveResource> Wave = MakeShared<FAudioGraphWaveResource>();
        Wave->SampleRate  = SampleRate;
        Wave->NumChannels = 1;
        Wave->NumFrames   = Frames;
        for (uint32 Frame = 0; Frame < Frames; ++Frame)
        {
            Wave->Samples.push_back((float)Frame * 0.01f);
        }
        return Wave;
    }

    uint16 AddGraphInput(FProgramBuilder& Builder, const char* Name, EAudioGraphType Type)
    {
        FAudioGraphParameterDecl Decl;
        Decl.Name = FName(Name);
        Decl.Type = Type;
        Decl.Slot = Builder.Slot(Type);
        Builder.Program.Inputs.push_back(Decl);
        return Decl.Slot;
    }

    void AddGraphOutput(FProgramBuilder& Builder, const char* Name, EAudioGraphType Type, uint16 Slot)
    {
        FAudioGraphParameterDecl Decl;
        Decl.Name = FName(Name);
        Decl.Type = Type;
        Decl.Slot = Slot;
        Builder.Program.Outputs.push_back(Decl);
    }

    uint16 WaveLiteral(FProgramBuilder& Builder, int32 WaveIndex)
    {
        FAudioGraphSlotInit Init;
        Init.Type      = EAudioGraphType::Wave;
        Init.Slot      = Builder.Slot(EAudioGraphType::Wave);
        Init.WaveIndex = WaveIndex;
        Builder.Program.SlotInits.push_back(Init);
        return Init.Slot;
    }

    SAudioGraphParameterValue ParameterValue(const char* Name, float Value)
    {
        SAudioGraphParameterValue Result;
        Result.Name  = FName(Name);
        Result.Value = Value;
        return Result;
    }

    TVector<float> RenderFrames(FAudioGraphInstance& Instance, uint32 Frames)
    {
        TVector<float> Output((size_t)Frames * 2, 0.0f);
        Instance.Render(Output.data(), Frames);
        return Output;
    }
}

// A looping wave plays to its loop end once, then repeats only the loop region, saying so on On Looped.
TEST(AudioGraph, WavePlayerLoopsItsRegion)
{
    // A power of two rate keeps the loop points exact in float seconds.
    constexpr uint32 kExactRate = 1024;

    FProgramBuilder Builder;
    const uint16 OnPlay = AddGraphInput(Builder, kAudioGraphOnPlayInput, EAudioGraphType::Trigger);

    FAudioGraphNodeInstance Player;
    Player.OperatorName = "WavePlayer";
    Player.InputSlots = {
        WaveLiteral(Builder, 0), OnPlay, Builder.Slot(EAudioGraphType::Trigger),
        Builder.Literal(EAudioGraphType::Bool, 0.0f, 0, true), Builder.Literal(EAudioGraphType::Float, 1.0f),
        Builder.Literal(EAudioGraphType::Float, 0.0f),
        Builder.Literal(EAudioGraphType::Float, 50.0f / (float)kExactRate),
        Builder.Literal(EAudioGraphType::Float, 25.0f / (float)kExactRate) };
    Player.OutputSlots = {
        Builder.Slot(EAudioGraphType::Audio), Builder.Slot(EAudioGraphType::Audio), Builder.Slot(EAudioGraphType::Trigger),
        Builder.Slot(EAudioGraphType::Trigger), Builder.Slot(EAudioGraphType::Float) };
    Builder.Program.OutputLeftSlot  = Player.OutputSlots[0];
    Builder.Program.OutputRightSlot = Player.OutputSlots[1];
    AddGraphOutput(Builder, "Looped", EAudioGraphType::Trigger, Player.OutputSlots[3]);
    Builder.Program.Nodes.push_back(Player);

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, { MakeRampWave(100, kExactRate) }, kExactRate, 2));

    const TVector<float> Output = RenderFrames(Instance, kAudioGraphBlockFrames);
    for (uint32 Frame = 0; Frame < 75; ++Frame)
    {
        ASSERT_NEAR(Output[Frame * 2], (float)Frame * 0.01f, 1.0e-5f) << Frame;
    }
    for (uint32 Frame = 75; Frame < kAudioGraphBlockFrames; ++Frame)
    {
        ASSERT_GE(Output[Frame * 2], 0.5f - 1.0e-5f) << Frame;
        ASSERT_LT(Output[Frame * 2], 0.75f) << Frame;
    }
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Looped")), 8u);
}

// A graph saved before the Wave Player gained its loop pins still builds and plays, reading zero for the new ones.
TEST(AudioGraph, ProgramFromBeforeAppendedPinsStillPlays)
{
    FProgramBuilder Builder;
    const uint16 OnPlay = AddGraphInput(Builder, kAudioGraphOnPlayInput, EAudioGraphType::Trigger);

    FAudioGraphNodeInstance Player;
    Player.OperatorName = "WavePlayer";
    Player.InputSlots = {
        WaveLiteral(Builder, 0), OnPlay, Builder.Slot(EAudioGraphType::Trigger),
        Builder.Literal(EAudioGraphType::Bool, 0.0f), Builder.Literal(EAudioGraphType::Float, 1.0f),
        Builder.Literal(EAudioGraphType::Float, 0.0f) };
    Player.OutputSlots = { Builder.Slot(EAudioGraphType::Audio), Builder.Slot(EAudioGraphType::Audio), Builder.Slot(EAudioGraphType::Trigger) };
    Builder.Program.OutputLeftSlot = Player.OutputSlots[0];
    Builder.Program.Nodes.push_back(Player);

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, { MakeRampWave(400) }, kTestSampleRate, 2));
    EXPECT_GT(PeakOf(RenderFrames(Instance, kAudioGraphBlockFrames)), 2.0f);
}

// Random Wave in no-repeat mode never hands out the same wave twice in a row, and sequential mode cycles.
TEST(AudioGraph, RandomWavePicksByMode)
{
    for (const EAudioPickMode Mode : { EAudioPickMode::RandomNoRepeat, EAudioPickMode::Sequential })
    {
        FProgramBuilder Builder;
        const uint16 Next = AddGraphInput(Builder, "Next", EAudioGraphType::Trigger);

        FAudioGraphNodeInstance Picker;
        Picker.OperatorName = "RandomWave";
        Picker.InputSlots = {
            Next, Builder.Literal(EAudioGraphType::Int32, 0.0f, (int32)Mode), Builder.Literal(EAudioGraphType::Int32, 0.0f),
            Builder.Literal(EAudioGraphType::Int32, 0.0f, 7), WaveLiteral(Builder, 0), WaveLiteral(Builder, 1), WaveLiteral(Builder, 2) };
        Picker.OutputSlots = { Builder.Slot(EAudioGraphType::Wave), Builder.Slot(EAudioGraphType::Int32) };

        FAudioGraphNodeInstance ToFloat;
        ToFloat.OperatorName = "IntToFloat";
        ToFloat.InputSlots   = { Picker.OutputSlots[1] };
        ToFloat.OutputSlots  = { Builder.Slot(EAudioGraphType::Float) };
        AddGraphOutput(Builder, "Picked", EAudioGraphType::Float, ToFloat.OutputSlots[0]);

        Builder.Program.Nodes.push_back(Picker);
        Builder.Program.Nodes.push_back(ToFloat);

        const TSharedPtr<FAudioGraphWaveResource> Wave = MakeRampWave(16);
        FAudioGraphInstance Instance;
        ASSERT_TRUE(Instance.Initialize(Builder.Program, { Wave, Wave, Wave }, kTestSampleRate, 2));

        RenderFrames(Instance, kAudioGraphBlockFrames);
        float Previous = Instance.GetFloatOutput(FName("Picked"));
        for (uint32 Step = 0; Step < 30; ++Step)
        {
            ASSERT_TRUE(Instance.TriggerParameter(FName("Next")));
            RenderFrames(Instance, kAudioGraphBlockFrames);

            const float Picked = Instance.GetFloatOutput(FName("Picked"));
            ASSERT_GE(Picked, 0.0f);
            ASSERT_LE(Picked, 2.0f);
            ASSERT_NE(Picked, Previous) << Step;
            if (Mode == EAudioPickMode::Sequential)
            {
                ASSERT_EQ(Picked, (float)(((uint32)Previous + 1) % 3));
            }
            Previous = Picked;
        }
    }
}

// Trigger Select in sequence over three outputs deals the triggers out evenly and leaves the fourth alone.
TEST(AudioGraph, TriggerSelectDealsInTurn)
{
    FProgramBuilder Builder;
    const uint16 Fire = AddGraphInput(Builder, "Fire", EAudioGraphType::Trigger);

    FAudioGraphNodeInstance& Select = Builder.AddNode("TriggerSelect");
    Select.InputSlots = {
        Fire, Builder.Slot(EAudioGraphType::Trigger), Builder.Literal(EAudioGraphType::Int32, 0.0f, (int32)EAudioPickMode::Sequential),
        Builder.Literal(EAudioGraphType::Int32, 0.0f), Builder.Literal(EAudioGraphType::Int32, 0.0f, 3), Builder.Literal(EAudioGraphType::Int32, 0.0f, 1) };
    for (uint32 Index = 0; Index < 4; ++Index)
    {
        Select.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Trigger));
    }
    Select.OutputSlots.push_back(Builder.Slot(EAudioGraphType::Int32));

    const char* OutputNames[] = { "Out0", "Out1", "Out2", "Out3" };
    for (uint32 Index = 0; Index < 4; ++Index)
    {
        AddGraphOutput(Builder, OutputNames[Index], EAudioGraphType::Trigger, Builder.Program.Nodes[0].OutputSlots[Index]);
    }

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));
    for (uint32 Step = 0; Step < 6; ++Step)
    {
        ASSERT_TRUE(Instance.TriggerParameter(FName("Fire")));
        RenderFrames(Instance, kAudioGraphBlockFrames);
    }

    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Out0")), 2u);
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Out1")), 2u);
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Out2")), 2u);
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Out3")), 0u);
}

// A full scale sine twenty decibels over a twenty to one threshold comes out near minus nineteen decibels.
TEST(AudioGraph, CompressorPullsALoudSignalDown)
{
    FProgramBuilder Builder;
    const uint16 Sine = Builder.AddSine(440.0f).OutputSlots[0];

    FAudioGraphNodeInstance& Compressor = Builder.AddNode("Compressor");
    Compressor.InputSlots = {
        Sine, Sine, Builder.Slot(EAudioGraphType::Audio), Builder.Literal(EAudioGraphType::Bool, 0.0f),
        Builder.Literal(EAudioGraphType::Float, -20.0f), Builder.Literal(EAudioGraphType::Float, 20.0f),
        Builder.Literal(EAudioGraphType::Float, 0.0f), Builder.Literal(EAudioGraphType::Float, 0.1f),
        Builder.Literal(EAudioGraphType::Float, 0.0f), Builder.Literal(EAudioGraphType::Float, 0.0f) };
    Compressor.OutputSlots = { Builder.Slot(EAudioGraphType::Audio), Builder.Slot(EAudioGraphType::Audio), Builder.Slot(EAudioGraphType::Float) };
    Builder.Program.OutputLeftSlot  = Compressor.OutputSlots[0];
    Builder.Program.OutputRightSlot = Compressor.OutputSlots[1];

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));

    RenderFrames(Instance, kTestSampleRate / 10);
    const float Peak = PeakOf(RenderFrames(Instance, 4096));
    EXPECT_GT(Peak, 0.08f);
    EXPECT_LT(Peak, 0.15f);
}

// At 120 BPM in four with two ticks a beat, one second holds four ticks, two beats and the opening bar.
TEST(AudioGraph, BeatClockKeepsTempo)
{
    FProgramBuilder Builder;
    const uint16 OnPlay = AddGraphInput(Builder, kAudioGraphOnPlayInput, EAudioGraphType::Trigger);

    FAudioGraphNodeInstance& Clock = Builder.AddNode("BeatClock");
    Clock.InputSlots = {
        OnPlay, Builder.Slot(EAudioGraphType::Trigger), Builder.Literal(EAudioGraphType::Float, 120.0f),
        Builder.Literal(EAudioGraphType::Int32, 0.0f, 4), Builder.Literal(EAudioGraphType::Int32, 0.0f, 2) };
    Clock.OutputSlots = {
        Builder.Slot(EAudioGraphType::Trigger), Builder.Slot(EAudioGraphType::Trigger),
        Builder.Slot(EAudioGraphType::Trigger), Builder.Slot(EAudioGraphType::Int32) };
    AddGraphOutput(Builder, "Tick", EAudioGraphType::Trigger, Builder.Program.Nodes[0].OutputSlots[0]);
    AddGraphOutput(Builder, "Beat", EAudioGraphType::Trigger, Builder.Program.Nodes[0].OutputSlots[1]);
    AddGraphOutput(Builder, "Bar", EAudioGraphType::Trigger, Builder.Program.Nodes[0].OutputSlots[2]);

    // Triggers publish per whole block, so stop at the last block boundary short of one second.
    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));
    RenderFrames(Instance, (kTestSampleRate / kAudioGraphBlockFrames) * kAudioGraphBlockFrames);

    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Tick")), 4u);
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Beat")), 2u);
    EXPECT_EQ(Instance.GetTriggerOutputCount(FName("Bar")), 1u);
}

// A parameter value lands as the input's own type, the way an audio source's overrides arrive.
TEST(AudioGraph, ParameterValueFollowsTheInputType)
{
    FProgramBuilder Builder;
    const uint16 Gain  = AddGraphInput(Builder, "Gain", EAudioGraphType::Float);
    const uint16 Count = AddGraphInput(Builder, "Count", EAudioGraphType::Int32);
    AddGraphInput(Builder, "Fire", EAudioGraphType::Trigger);

    FAudioGraphNodeInstance& ToFloat = Builder.AddNode("IntToFloat");
    ToFloat.InputSlots  = { Count };
    ToFloat.OutputSlots = { Builder.Slot(EAudioGraphType::Float) };
    AddGraphOutput(Builder, "GainOut", EAudioGraphType::Float, Gain);
    AddGraphOutput(Builder, "CountOut", EAudioGraphType::Float, Builder.Program.Nodes[0].OutputSlots[0]);

    FAudioGraphInstance Instance;
    ASSERT_TRUE(Instance.Initialize(Builder.Program, {}, kTestSampleRate, 2));

    EXPECT_TRUE(Instance.SetParameter(ParameterValue("Gain", 0.25f)));
    EXPECT_TRUE(Instance.SetParameter(ParameterValue("Count", 2.6f)));
    EXPECT_TRUE(Instance.SetParameter(ParameterValue("Fire", 1.0f)));
    EXPECT_FALSE(Instance.SetParameter(ParameterValue("Fire", 0.0f)));
    EXPECT_FALSE(Instance.SetParameter(ParameterValue("Missing", 1.0f)));

    RenderFrames(Instance, kAudioGraphBlockFrames);
    EXPECT_FLOAT_EQ(Instance.GetFloatOutput(FName("GainOut")), 0.25f);
    EXPECT_FLOAT_EQ(Instance.GetFloatOutput(FName("CountOut")), 3.0f);
}
