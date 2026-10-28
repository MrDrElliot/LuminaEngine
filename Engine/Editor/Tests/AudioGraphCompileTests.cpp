#include <gtest/gtest.h>

#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/Graph/AudioGraphInstance.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphCompiler.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"
#include "UI/Tools/NodeGraph/Audio/AudioNodeGraph.h"
#include "UI/Tools/NodeGraph/Audio/Nodes/AudioNodes_Generators.h"

using namespace Lumina;

namespace
{
    CAudioNodeGraph* MakeAudioNodeGraph(const char* PackagePath)
    {
        // The editor makes every default object at startup, which links each class's properties for serialization.
        CAudioNodeGraph::StaticClass()->GetDefaultObject();
        CAudioGraphInputNode::StaticClass()->GetDefaultObject();
        CAudioGraphOutputNode::StaticClass()->GetDefaultObject();
        CAudioNode_WavePlayer::StaticClass()->GetDefaultObject();
        CAudioNode_RandomWave::StaticClass()->GetDefaultObject();

        CAudioNodeGraph* Graph = NewObject<CAudioNodeGraph>(CPackage::CreatePackage(PackagePath), "Graph");
        Graph->Initialize();
        return Graph;
    }
}

// A dropped wave is wired to play the moment the graph does, which a new graph's On Play trigger makes possible.
TEST(AudioGraphCompile, DroppedWavePlaysOnPlay)
{
    CAudioNodeGraph* Graph = MakeAudioNodeGraph("/AudioGraphCompile/Drop");

    CAudioGraphInputNode* OnPlay = Graph->FindOnPlayNode();
    ASSERT_NE(OnPlay, nullptr);
    EXPECT_EQ(OnPlay->Type, EAudioGraphType::Trigger);

    // Opening the graph again must not stack a second one.
    Graph->Initialize();
    uint32 OnPlayCount = 0;
    for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
    {
        const CAudioGraphInputNode* Input = Cast<CAudioGraphInputNode>(Node.Get());
        OnPlayCount += (Input != nullptr && Input->ParameterName == FName(kAudioGraphOnPlayInput)) ? 1u : 0u;
    }
    EXPECT_EQ(OnPlayCount, 1u);

    CAudioNode_WavePlayer* Player = Cast<CAudioNode_WavePlayer>(Graph->CreateNode(CAudioNode_WavePlayer::StaticClass()));
    ASSERT_NE(Player, nullptr);
    Graph->WireDroppedWavePlayer(Player);

    const FAudioGraphCompileResult Result = FAudioGraphCompiler::Compile(Graph);
    ASSERT_TRUE(Result.bSuccess);

    const FAudioGraphParameterDecl* OnPlayDecl = Result.Program.FindInput(FName(kAudioGraphOnPlayInput));
    ASSERT_NE(OnPlayDecl, nullptr);
    ASSERT_EQ(Result.Program.Nodes.size(), 1u);

    const FAudioGraphNodeInstance& Compiled = Result.Program.Nodes[0];
    EXPECT_EQ(Compiled.InputSlots[1], OnPlayDecl->Slot);
    EXPECT_EQ(Result.Program.OutputLeftSlot, Compiled.OutputSlots[0]);
    EXPECT_EQ(Result.Program.OutputRightSlot, Compiled.OutputSlots[1]);
    EXPECT_EQ(Result.Program.FinishedSlot, Compiled.OutputSlots[2]);

    // A second drop leaves the Output wired to the first.
    CAudioNode_WavePlayer* Second = Cast<CAudioNode_WavePlayer>(Graph->CreateNode(CAudioNode_WavePlayer::StaticClass()));
    Graph->WireDroppedWavePlayer(Second);
    EXPECT_EQ(Graph->FindOutputNode()->GetInputPins()[0]->GetConnection(0)->GetOwningNode(), Player);
}

// A Random Wave's list compiles to wave slots after its pins, skipping empty entries and sharing repeats.
TEST(AudioGraphCompile, RandomWaveListBecomesWaveSlots)
{
    CAudioNodeGraph* Graph = MakeAudioNodeGraph("/AudioGraphCompile/RandomWave");
    CPackage* Package = Graph->GetPackage();

    CAudioStream* WaveA = NewObject<CAudioStream>(Package, "WaveA");
    CAudioStream* WaveB = NewObject<CAudioStream>(Package, "WaveB");

    CAudioNode_RandomWave* Picker = Cast<CAudioNode_RandomWave>(Graph->CreateNode(CAudioNode_RandomWave::StaticClass()));
    CAudioNode_WavePlayer* Player = Cast<CAudioNode_WavePlayer>(Graph->CreateNode(CAudioNode_WavePlayer::StaticClass()));
    ASSERT_NE(Picker, nullptr);
    ASSERT_NE(Player, nullptr);

    Picker->Waves.push_back(WaveA);
    Picker->Waves.push_back(TStrongObjectPtr<CAudioStream>());
    Picker->Waves.push_back(WaveB);
    Picker->Waves.push_back(WaveA);

    ASSERT_TRUE(Graph->ConnectPins(Picker->GetOutputPins()[0].Get(), Player->GetInputPins()[0].Get()));
    Graph->WireDroppedWavePlayer(Player);

    const FAudioGraphCompileResult Result = FAudioGraphCompiler::Compile(Graph);
    ASSERT_TRUE(Result.bSuccess);
    EXPECT_TRUE(Result.Warnings.empty());

    ASSERT_EQ(Result.Waves.size(), 2u);
    EXPECT_EQ(Result.Waves[0].Get(), WaveA);
    EXPECT_EQ(Result.Waves[1].Get(), WaveB);

    const FAudioGraphNodeInstance* Compiled = nullptr;
    for (const FAudioGraphNodeInstance& Node : Result.Program.Nodes)
    {
        Compiled = Node.OperatorName == FName("RandomWave") ? &Node : Compiled;
    }
    ASSERT_NE(Compiled, nullptr);
    ASSERT_EQ(Compiled->InputSlots.size(), Picker->GetInputPins().size() + 3u);

    TVector<int32> ListedWaves;
    for (size_t Index = Picker->GetInputPins().size(); Index < Compiled->InputSlots.size(); ++Index)
    {
        for (const FAudioGraphSlotInit& Init : Result.Program.SlotInits)
        {
            if (Init.Type == EAudioGraphType::Wave && Init.Slot == Compiled->InputSlots[Index])
            {
                ListedWaves.push_back(Init.WaveIndex);
            }
        }
    }
    EXPECT_TRUE(ListedWaves == (TVector<int32>{ 0, 1, 0 }));

    Picker->Waves.clear();
    const FAudioGraphCompileResult Empty = FAudioGraphCompiler::Compile(Graph);
    EXPECT_TRUE(Empty.bSuccess);
    EXPECT_EQ(Empty.Warnings.size(), 1u);
}
